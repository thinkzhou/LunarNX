#ifdef __SWITCH__

#include "ps_stream_controller.h"
#include "ps_media_bridge.h"
#include "ps_console_resolver.h"
#include "psn_auth_utils.h"
#include "chiaki_log_adapter.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"
#include <chrono>
#include <cstring>
#include <thread>

namespace lunar::ps {

PsStreamController::PsStreamController(const PsConsole& console,
                                        const std::string& psn_access_token,
                                        const std::string& psn_account_id,
                                        int width, int height, int fps, int bitrate_kbps)
    : console_(console)
    , psn_access_token_(psn_access_token)
    , psn_account_id_(psn_account_id)
    , width_(width)
    , height_(height)
    , fps_(fps)
    , bitrate_kbps_(bitrate_kbps)
    , video_backend_(stream::VideoBackend::HardwareZeroCopy) {}

PsStreamController::~PsStreamController() {
    requestCancel();
    stopStream(false);
    std::unique_lock<std::shared_mutex> lock(stream_operation_mutex_);
    media_.reset();
}

bool PsStreamController::startStream() {
    std::unique_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
    if (state_.load() != app::StreamState::Idle) return false;
    if (cancel_requested_.load()) return false;
    setState(app::StreamState::Connecting, "Setting up stream...");
    stream_transport_connected_ = false;
    last_input_buttons_ = 0;
    input_transition_logs_ = 0;
    lunar::startDropDiagnosticWriter();

    const bool mock_replay = psMockReplayEnabled();
    bool has_token = !psn_access_token_.empty();
    ResolvedRoute route;
    if (mock_replay) {
        route.type = ResolvedRouteType::Local;
        route.host_addr = "mock-replay";
        diagnosticLog("ps-controller", "development mock replay selected");
    } else {
        route = PsConsoleResolver::resolve(console_, has_token);
    }

    if (route.type == ResolvedRouteType::None) {
        last_error_ = route.error.empty() ? "No route available" : route.error;
        setState(app::StreamState::Error, last_error_);
        return false;
    }

    // Remote path: punch holes
    if (!mock_replay && route.type == ResolvedRouteType::Remote) {
        setState(app::StreamState::Connecting, "Connecting via PSN...");

        diagnosticLog("ps-controller", "remote target=%d ps5=%d duid=%s",
                      console_.target, console_.target >= 1000000 ? 1 : 0,
                      route.console_duid.c_str());

        std::string account_id_bytes;
        if (!base64Decode(psn_account_id_, account_id_bytes) ||
            account_id_bytes.size() != CHIAKI_PSN_ACCOUNT_ID_SIZE) {
            last_error_ = "PSN account ID is missing or invalid";
            setState(app::StreamState::Error, last_error_);
            return false;
        }
        remote_result_.psn_account_id = std::move(account_id_bytes);

        remote_log_ = makeChiakiDiagnosticLog("chiaki-remote");
        {
            std::lock_guard<std::mutex> lock(remote_connector_mutex_);
            remote_connector_ =
                std::make_unique<PsRemoteConnector>(psn_access_token_, &remote_log_);
        }
        if (cancel_requested_.load()) return false;

        uint8_t console_uid[32]{};
        if (!decodeDuid(route.console_duid, console_uid)) {
            last_error_ = "PSN console ID is invalid";
            setState(app::StreamState::Error, last_error_);
            return false;
        }

        PsRemoteConnector* connector = nullptr;
        {
            std::lock_guard<std::mutex> lock(remote_connector_mutex_);
            connector = remote_connector_.get();
        }
        if (!connector->connect(console_.target, console_uid,
                [this](const std::string& phase) {
                    setState(app::StreamState::Connecting, phase);
                },
                remote_result_)) {
            last_error_ = "Remote connection failed";
            setState(app::StreamState::Error, last_error_);
            return false;
        }
    }

    // Create long-lived components (lightweight constructors only — heavy init
    // is deferred until after the chiaki session thread is running, so the PS5
    // does not time out waiting for regist/request messages on the CTRL channel).
    stream_backend_ = stream::StreamBackendProvider::createDefault();
    media_ = std::make_unique<stream::MediaPipeline>(*stream_backend_);
    gamepad_ = std::make_unique<input::GamepadReader>();
    input_mapper_ = std::make_unique<PsInputMapper>();
    if (gamepad_) gamepad_->initialize();

    uint8_t regist_key[0x10]{};
    uint8_t morning[0x10]{};
    if (!mock_replay && route.type == ResolvedRouteType::Local) {
        if (!console_.credentials.has_value()) {
            last_error_ = "Console is not paired for local play";
            setState(app::StreamState::Error, last_error_);
            return false;
        }
        std::memcpy(regist_key, console_.credentials->rp_regist_key,
                    sizeof(regist_key));
        std::memcpy(morning, console_.credentials->rp_key, sizeof(morning));
    }
    std::string host_addr = route.type == ResolvedRouteType::Local
        ? route.host_addr : "";

    media_->setVideoReadyCallback([this]() {
        setState(app::StreamState::Streaming, "Video ready");
    });
    bridge_ = std::make_unique<PsMediaBridge>(*media_, mock_replay ? 30 : fps_);
    if (mock_replay) {
        mock_session_ = std::make_unique<PsMockReplaySession>(*bridge_, fps_);
    } else {
        session_ = std::make_unique<PsStreamSession>(
            host_addr, regist_key, morning, console_.target,
            width_, height_, fps_, bitrate_kbps_, *bridge_);
    }

    PsSessionCallbacks callbacks;
    callbacks.on_status = [this](const std::string& info) {
        setState(app::StreamState::Connecting, info);
    };
    callbacks.on_streaming = [this]() {
        stream_transport_connected_ = true;
        setState(app::StreamState::Connecting, "Connected. Waiting for video...");
        // The media pipeline may have been ready long before PSN DATA setup
        // completed. Request again at the first point StreamConnection can
        // actually deliver the fresh SPS/PPS/IDR.
        requestRecoveryIDR();
    };
    callbacks.on_login_pin_requested = [this](bool incorrect) {
        setState(app::StreamState::Connecting,
                 incorrect ? "Console login PIN incorrect"
                           : "Console login PIN required");
        LoginPinCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = login_pin_callback_;
        }
        if (callback) callback(incorrect);
    };
    callbacks.on_error = [this](const std::string& err) {
        last_error_ = err;
        setState(app::StreamState::Error, err);
    };
    callbacks.on_disconnected = [this](const std::string& reason) {
        setState(app::StreamState::Disconnected, reason);
    };
    callbacks.external_cancel = [this]() { return cancel_requested_.load(); };

    setState(app::StreamState::Connecting, "Connecting to PlayStation...");

    // Start the chiaki session immediately so the session thread begins
    // regist/request/ctrl over the punched CTRL channel without delay.
    bool ok;
    if (mock_replay) {
        ok = mock_session_->start(std::move(callbacks));
    } else if (route.type == ResolvedRouteType::Remote) {
        ok = session_->startRemote(std::move(remote_result_), std::move(callbacks));
    } else {
        ok = session_->start(std::move(callbacks));
    }

    if (!ok) {
        last_error_ = mock_replay ? mock_session_->lastError()
                                  : session_->lastError();
        setState(app::StreamState::Error, last_error_);
        return false;
    }

    // Now initialize the media pipeline (NVDEC, audio) while the session
    // thread already runs regist/request on the CTRL channel in parallel.
    stream::MediaPipelineOptions media_opts;
    media_opts.video_backend = video_backend_;
    if (!media_->initialize(width_, height_, &perf_, media_opts)) {
        last_error_ = "Failed to initialize media pipeline";
        setState(app::StreamState::Error, last_error_);
        return false;
    }

    // The PS5 may begin sending immediately after CHIAKI_EVENT_CONNECTED,
    // while NVDEC/Audren are still being initialized. Always request a fresh
    // SPS/PPS/IDR once the media pipeline is ready.
    if (mock_session_) mock_session_->requestIDR();
    else session_->requestIDR();
    diagnosticLog("ps-controller", "media ready; requested initial IDR");
    startVideoMonitor();

    return true;
}

void PsStreamController::startVideoMonitor() {
    stopVideoMonitor();
    video_monitor_stop_ = false;
    video_monitor_thread_ = std::thread([this]() {
        std::chrono::steady_clock::time_point waiting_started{};
        bool waiting_notice_sent = false;
        while (!video_monitor_stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (video_monitor_stop_.load()) break;
            if (state_.load() == app::StreamState::Streaming) break;
            if (!stream_transport_connected_.load()) continue;
            if (waiting_started.time_since_epoch().count() == 0) {
                waiting_started = std::chrono::steady_clock::now();
            }

            if (media_ && media_->hasVideoRecoveryRequest() &&
                requestRecoveryIDR()) {
                media_->clearVideoRecoveryRequest();
                diagnosticLog("ps-controller",
                              "requested IDR while waiting for first rendered frame");
            }

            if (!waiting_notice_sent &&
                std::chrono::steady_clock::now() - waiting_started >=
                    std::chrono::seconds(10)) {
                waiting_notice_sent = true;
                const bool received_access_units = perf_.video_packets.load() > 0;
                const char* info = received_access_units
                    ? "Video received. Recovering decoder..."
                    : "Connected. Still waiting for video packets...";
                diagnosticLog("ps-controller",
                              "first rendered frame timeout access_units=%u decode_errors=%u",
                              perf_.video_packets.load(),
                              perf_.video_decode_errors.load());
                setState(app::StreamState::Connecting, info);
            }
        }
    });
}

bool PsStreamController::requestRecoveryIDR() {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(video_recovery_mutex_);
        if (last_recovery_request_.time_since_epoch().count() != 0 &&
            now - last_recovery_request_ < std::chrono::seconds(1)) {
            return false;
        }
        last_recovery_request_ = now;
    }
    if (mock_session_) mock_session_->requestIDR();
    else if (session_) session_->requestIDR();
    else return false;
    return true;
}

void PsStreamController::stopVideoMonitor() {
    video_monitor_stop_ = true;
    if (video_monitor_thread_.joinable() &&
        video_monitor_thread_.get_id() != std::this_thread::get_id()) {
        video_monitor_thread_.join();
    }
}

void PsStreamController::requestCancel() {
    cancel_requested_ = true;
    if (mock_session_) mock_session_->stop();
    std::lock_guard<std::mutex> lock(remote_connector_mutex_);
    if (remote_connector_) remote_connector_->cancel();
}

void PsStreamController::stopStream(bool set_disconnected) {
    if (shutdown_.exchange(true)) return;
    stopVideoMonitor();
    std::unique_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
    cancel_requested_ = true;
    if (mock_session_) { mock_session_->stop(); mock_session_.reset(); }
    if (session_) { session_->stop(); session_.reset(); }
    {
        std::lock_guard<std::mutex> remote_lock(remote_connector_mutex_);
        if (remote_connector_) {
            remote_connector_->cancel();
            remote_connector_.reset();
        }
    }
    bridge_.reset();
    if (media_) {
        media_->setVideoReadyCallback({});
        media_->shutdown();
    }
    lunar::stopDropDiagnosticWriter();
    if (set_disconnected) setState(app::StreamState::Disconnected, "Stopped");
}

void PsStreamController::update() {
    std::shared_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
    if ((!session_ && !mock_session_) ||
        state_.load() != app::StreamState::Streaming) return;
    if (!gamepad_ || !input_mapper_) return;

    auto state = gamepad_->read();
    if (ps_button_requested_.exchange(false)) state.guide = true;
    if (input_suppressed_.load()) state = {};

    ChiakiControllerState cs = input_mapper_->map(state);
    if (mock_session_) mock_session_->setControllerState(cs);
    else session_->setControllerState(cs);
    perf_.recordInputPacket();
    if (cs.buttons != last_input_buttons_) {
        // Release builds use APP_DIAG=0. Preserve a small amount of real-input
        // evidence through the asynchronous writer without synchronously
        // touching the SD card from the 8 ms input loop.
        if (input_transition_logs_ < 64) {
            lunar::dropDiagnosticLog(
                "ps-input",
                "buttons old=0x%x new=0x%x lx=%d ly=%d rx=%d ry=%d l2=%u r2=%u",
                last_input_buttons_, cs.buttons,
                cs.left_x, cs.left_y, cs.right_x, cs.right_y,
                cs.l2_state, cs.r2_state);
            input_transition_logs_++;
        }
        last_input_buttons_ = cs.buttons;
    }

    if (media_ && media_->hasVideoRecoveryRequest()) {
        if (requestRecoveryIDR()) media_->clearVideoRecoveryRequest();
    }
}

void PsStreamController::presentVideoFrame() {
    std::shared_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
    if (media_) media_->presentVideoFrame();
}

void PsStreamController::setState(app::StreamState s, const std::string& info) {
    state_.store(s);
    if (s == app::StreamState::Error && !info.empty()) last_error_ = info;
    LaunchCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        launch_info_ = info;
        callback = launch_callback_;
    }
    if (callback) callback(s, info);
}

void PsStreamController::setLaunchCallback(LaunchCallback cb) {
    app::StreamState state = state_.load();
    std::string info;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        launch_callback_ = std::move(cb);
        info = launch_info_;
    }
    LaunchCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = launch_callback_;
    }
    if (callback) callback(state, info);
}

void PsStreamController::setLoginPinCallback(LoginPinCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    login_pin_callback_ = std::move(cb);
}

void PsStreamController::submitLoginPin(const std::string& pin) {
    std::shared_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
    if (session_) session_->setLoginPin(pin);
}

} // namespace lunar::ps

#endif
