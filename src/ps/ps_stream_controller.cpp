#ifdef __SWITCH__

#include "ps_stream_controller.h"
#include "ps_media_bridge.h"
#include "ps_console_resolver.h"
#include "psn_auth_utils.h"
#include "chiaki_log_adapter.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <thread>

#ifndef LUNARNX_PS_DIRECT_VIDEO
#define LUNARNX_PS_DIRECT_VIDEO 0
#endif

namespace lunar::ps {

namespace {
constexpr int kPsButtonPulseFrames = 16;
constexpr std::chrono::milliseconds kPsInputInterval{8};
}

void PsStreamController::releasePendingRemoteResult() {
    if (remote_result_.holepunch_session) {
        chiaki_holepunch_session_fini(remote_result_.holepunch_session);
    }
    remote_result_ = {};
}

PsStreamController::PsStreamController(const PsConsole& console,
                                        const std::string& psn_access_token,
                                        const std::string& psn_account_id,
                                        int width, int height, int fps, int bitrate_kbps,
                                        stream::VideoCodec video_codec)
    : console_(console)
    , psn_access_token_(psn_access_token)
    , psn_account_id_(psn_account_id)
    , width_(width)
    , height_(height)
    , fps_(fps)
    , bitrate_kbps_(bitrate_kbps)
    , video_codec_(console.target >= 1000000
          ? video_codec : stream::VideoCodec::H264)
    , video_backend_(stream::VideoBackend::HardwareZeroCopy) {}

PsStreamController::~PsStreamController() {
    requestCancel();
    stopStream(false);
    std::unique_lock<std::shared_mutex> lock(stream_operation_mutex_);
    media_.reset();
}

void PsStreamController::setRumbleEnabled(bool enabled) {
    rumble_enabled_ = enabled;
    if (rumble_) rumble_->setEnabled(enabled);
}

void PsStreamController::setRumbleStrengthPercent(int percent) {
    percent = std::clamp(percent, 0, 100);
    rumble_strength_percent_ = percent;
    if (rumble_) rumble_->setStrengthPercent(percent);
}

bool PsStreamController::setPsnCredentials(std::string access_token,
                                            std::string account_id) {
    std::unique_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
    if (state_.load() != app::StreamState::Idle) return false;
    psn_access_token_ = std::move(access_token);
    psn_account_id_ = std::move(account_id);
    return true;
}

app::TouchpadFeedback PsStreamController::getTouchpadFeedback() const {
    std::lock_guard<std::mutex> lock(touchpad_feedback_mutex_);
    return touchpad_feedback_;
}

bool PsStreamController::startStream() {
    std::unique_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
    if (state_.load() != app::StreamState::Idle) return false;
    if (cancel_requested_.load()) return false;
    setState(app::StreamState::Connecting, "Setting up stream...");
    stream_transport_connected_ = false;
    ps_button_requested_ = false;
    ps_button_pulse_frames_remaining_ = 0;
    ps_button_release_pending_ = false;
    last_input_buttons_ = 0;
    input_transition_logs_ = 0;
    lunar::startDropDiagnosticWriter();

    const bool mock_replay = psMockReplayEnabled();
    diagnosticLog("ps-controller", "video codec=%s target_ps5=%d",
                  stream::videoCodecName(video_codec_),
                  console_.target >= 1000000 ? 1 : 0);
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
        if (cancel_requested_.load()) {
            releasePendingRemoteResult();
            return false;
        }

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
            if (cancel_requested_.load() ||
                remote_result_.error == CHIAKI_ERR_CANCELED) {
                return false;
            }
            last_error_ = "Remote connection failed";
            if (remote_result_.attempts > 1) {
                last_error_ += " after " + std::to_string(remote_result_.attempts) +
                               " attempts";
            }
            if (!remote_result_.failed_phase.empty()) {
                last_error_ += " at " + remote_result_.failed_phase;
            }
            if (remote_result_.error != CHIAKI_ERR_SUCCESS) {
                last_error_ += ": ";
                last_error_ += chiaki_error_string(remote_result_.error);
            }
            setState(app::StreamState::Error, last_error_);
            return false;
        }
        if (cancel_requested_.load()) {
            releasePendingRemoteResult();
            return false;
        }
    }

    // Create long-lived components (lightweight constructors only — heavy init
    // is deferred until after the chiaki session thread is running, so the PS5
    // does not time out waiting for regist/request messages on the CTRL channel).
    stream_backend_ = stream::StreamBackendProvider::createDefault();
    media_ = std::make_unique<stream::MediaPipeline>(*stream_backend_);
    gamepad_ = std::make_unique<input::GamepadReader>(
        input::ButtonMappingProfile::PlayStation);
    rumble_ = std::make_unique<input::RumbleController>();
    input_mapper_ = std::make_unique<PsInputMapper>();
    touchpad_reader_ = std::make_unique<PsTouchpadReader>(console_.target >= 1000000);
    motion_reader_ = std::make_unique<PsMotionReader>();
    if (gamepad_) gamepad_->initialize();
    if (motion_reader_) motion_reader_->initialize();
    if (rumble_) {
        rumble_->setEnabled(rumble_enabled_.load());
        rumble_->setStrengthPercent(rumble_strength_percent_.load());
        rumble_->initialize();
    }

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
    bridge_->setRumbleForwarder([this](uint8_t left, uint8_t right) {
        if (!rumble_ || !input_router_.gameHasInput() ||
            state_.load() != app::StreamState::Streaming) return;
        rumble_->setRumble(0,
            static_cast<float>(left) / 255.0f,
            static_cast<float>(right) / 255.0f,
            0.0f, 0.0f, 30, 0, 0);
    });
    if (mock_replay) {
        mock_session_ = std::make_unique<PsMockReplaySession>(
            *bridge_, fps_, video_codec_);
    } else {
        session_ = std::make_unique<PsStreamSession>(
            host_addr, regist_key, morning, console_.target,
            width_, height_, fps_, bitrate_kbps_, video_codec_, *bridge_);
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

    if (cancel_requested_.load()) return false;

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
        if (mock_session_) mock_session_->stop();
        else if (session_) session_->stop();
        if (cancel_requested_.load()) return false;
        setState(app::StreamState::Error, last_error_);
        return false;
    }
    if (cancel_requested_.load()) {
        if (mock_session_) mock_session_->stop();
        else session_->stop();
        return false;
    }

    // Now initialize the media pipeline (NVDEC, audio) while the session
    // thread already runs regist/request on the CTRL channel in parallel.
    stream::MediaPipelineOptions media_opts;
    media_opts.video_path = stream::VideoPipelinePath::PlayStation;
    media_opts.video_codec = video_codec_;
    media_opts.hold_non_target_startup_frames = true;
    media_opts.video_backend = video_backend_;
#if LUNARNX_PS_DIRECT_VIDEO
    media_opts.video_scheduling =
        stream::VideoSchedulingMode::DirectLowLatency;
#else
    media_opts.video_scheduling =
        stream::VideoSchedulingMode::BoundedLowLatency;
    media_opts.video_queue_limits.max_packets = std::clamp<size_t>(
        static_cast<size_t>((fps_ + 9) / 10), 3, 8);
    media_opts.video_queue_limits.max_bytes = 8 * 1024 * 1024;
    media_opts.video_queue_limits.max_age = std::chrono::milliseconds(100);
#endif
    if (!media_->initialize(width_, height_, &perf_, media_opts)) {
        last_error_ = "Failed to initialize media pipeline";
        if (mock_session_) mock_session_->stop();
        else if (session_) session_->stop();
        setState(app::StreamState::Error, last_error_);
        return false;
    }

    // The PS5 may begin sending immediately after CHIAKI_EVENT_CONNECTED,
    // while NVDEC/Audren are still being initialized. Always request a fresh
    // SPS/PPS/IDR once the media pipeline is ready.
    if (mock_session_) mock_session_->requestIDR();
    else session_->requestIDR();
    diagnosticLog("ps-controller", "media ready; requested initial IDR");
    startInputLoop();
    startVideoMonitor();

    return true;
}

void PsStreamController::startInputLoop() {
    stopInputLoop();
    input_loop_stop_ = false;
    input_thread_ = std::thread([this]() {
        auto next_tick = std::chrono::steady_clock::now();
        while (!input_loop_stop_.load()) {
            update();
            next_tick += kPsInputInterval;
            const auto now = std::chrono::steady_clock::now();
            if (next_tick <= now) next_tick = now + kPsInputInterval;
            std::this_thread::sleep_until(next_tick);
        }
    });
}

void PsStreamController::stopInputLoop() {
    input_loop_stop_ = true;
    if (input_thread_.joinable() &&
        input_thread_.get_id() != std::this_thread::get_id()) {
        input_thread_.join();
    }
}

void PsStreamController::startVideoMonitor() {
    stopVideoMonitor();
    video_monitor_stop_ = false;
    video_monitor_thread_ = std::thread([this]() {
        std::chrono::steady_clock::time_point waiting_started{};
        bool waiting_notice_sent = false;
        auto last_video_summary = std::chrono::steady_clock::now();
        auto last_video_detail_summary = last_video_summary;
        uint32_t previous_video_packets = 0;
        uint64_t previous_video_bytes = 0;
        while (!video_monitor_stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (video_monitor_stop_.load()) break;
            if (!stream_transport_connected_.load()) continue;
            const auto now = std::chrono::steady_clock::now();
            const bool streaming = state_.load() == app::StreamState::Streaming;
            if (streaming) {
                waiting_started = {};
                waiting_notice_sent = false;
            }
            if (waiting_started.time_since_epoch().count() == 0) {
                waiting_started = now;
            }

            if (media_ && media_->hasVideoRecoveryRequest() &&
                requestRecoveryIDR()) {
                media_->clearVideoRecoveryRequest();
                diagnosticLog("ps-controller",
                              "requested IDR for video recovery");
            }

            if (!waiting_notice_sent &&
                    now - waiting_started >=
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

            if (now - last_video_summary >= std::chrono::seconds(1)) {
                const uint32_t packets = perf_.video_packets.load();
                const uint64_t bytes = perf_.encoded_video_bytes.load();
                diagnosticLog(
                    "ps-media",
                    "1s video packets=%u bytes=%llu frames_lost=%u "
                    "enqueue_running=%d",
                    packets - previous_video_packets,
                    static_cast<unsigned long long>(bytes - previous_video_bytes),
                    perf_.ps_frames_lost.load(),
                    media_ && media_->isRunning() ? 1 : 0);
                previous_video_packets = packets;
                previous_video_bytes = bytes;
                last_video_summary = now;
            }
            if (now - last_video_detail_summary >= std::chrono::seconds(10)) {
                diagnosticLog(
                    "ps-media",
                    "10s decoded_queue_drop_oldest=%u decoded_queue_depth_high=%u "
                    "unique_submitted=%u new_frame_gap_max_us=%llu",
                    perf_.decoded_pending_drop_oldest.load(),
                    perf_.decoded_pending_depth_high.load(),
                    perf_.unique_video_frames_submitted.load(),
                    static_cast<unsigned long long>(
                        perf_.new_frame_submit_gap_max_us.load()));
                last_video_detail_summary = now;
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
    std::lock_guard<std::mutex> lock(remote_connector_mutex_);
    if (remote_connector_) remote_connector_->cancel();
}

void PsStreamController::stopStream(bool set_disconnected) {
    if (shutdown_.exchange(true)) {
        diagnosticLog("ps-controller", "stop stream skipped: already stopped");
        return;
    }
    diagnosticLog("ps-controller", "stop stream begin state=%d",
                  static_cast<int>(state_.load()));
    stopInputLoop();
    diagnosticLog("ps-controller", "input loop stopped");
    stopVideoMonitor();
    diagnosticLog("ps-controller", "video monitor stopped");
    std::unique_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
    cancel_requested_ = true;
    if (mock_session_) {
        mock_session_->stop();
        mock_session_.reset();
        diagnosticLog("ps-controller", "mock session stopped");
    }
    if (session_) {
        session_->stop();
        session_.reset();
        diagnosticLog("ps-controller", "chiaki session stopped and finalized");
    }
    {
        std::lock_guard<std::mutex> remote_lock(remote_connector_mutex_);
        if (remote_connector_) {
            remote_connector_->cancel();
            remote_connector_.reset();
            diagnosticLog("ps-controller", "remote connector released");
        }
    }
    releasePendingRemoteResult();
    diagnosticLog("ps-controller", "pending holepunch released");
    bridge_.reset();
    if (media_) {
        media_->setVideoReadyCallback({});
        media_->shutdown();
        diagnosticLog("ps-controller", "media pipeline stopped");
    }
    if (rumble_) rumble_->stop();
    if (gamepad_) gamepad_->releaseCaptureButton();
    lunar::stopDropDiagnosticWriter();
    ps_button_requested_ = false;
    ps_button_pulse_frames_remaining_ = 0;
    ps_button_release_pending_ = false;
    if (touchpad_reader_) touchpad_reader_->reset();
    if (motion_reader_) motion_reader_->reset();
    {
        std::lock_guard<std::mutex> lock(touchpad_feedback_mutex_);
        touchpad_feedback_ = {};
    }
    if (input_mapper_) input_mapper_->reset();
    if (set_disconnected) setState(app::StreamState::Disconnected, "Stopped");
    diagnosticLog("ps-controller", "stop stream complete");
}

bool PsStreamController::resumeAfterForeground() {
    {
        std::shared_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
        if (state_.load() == app::StreamState::Streaming &&
            stream_transport_connected_.load()) {
            const bool requested = requestRecoveryIDR();
            diagnosticLog("ps-controller",
                          "foreground resume kept healthy session idr=%d",
                          requested ? 1 : 0);
            return true;
        }
    }

    diagnosticLog("ps-controller", "foreground resume rebuilding PS session");
    stopStream(false);
    shutdown_ = false;
    cancel_requested_ = false;
    setState(app::StreamState::Idle, "Resuming stream...");
    return startStream();
}

void PsStreamController::update() {
    std::shared_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
    if ((!session_ && !mock_session_) ||
        state_.load() != app::StreamState::Streaming) return;
    if (!gamepad_ || !input_mapper_) return;

    const auto now = std::chrono::steady_clock::now();
    if (session_ && (last_stats_sample_.time_since_epoch().count() == 0 ||
        now - last_stats_sample_ >= std::chrono::milliseconds(500))) {
        const auto stats = session_->transportStats();
        perf_.recordPsTransportStats(stats.rtt_ms, stats.measured_bitrate_mbps,
                                     stats.packet_loss_fraction,
                                     stats.frames_lost);
        last_stats_sample_ = now;
    }

    auto state = gamepad_->read();
    const bool input_suppressed = !input_router_.gameHasInput();
    PsTouchpadState touchpad = touchpad_reader_
        ? touchpad_reader_->read(input_suppressed)
        : PsTouchpadState{};
    if (touchpad_reader_) {
        const auto feedback = touchpad_reader_->feedback();
        app::TouchpadFeedback snapshot{};
        switch (feedback.gesture) {
            case PsTouchpadGesture::Touch:
                snapshot.gesture = app::TouchpadFeedbackGesture::Touch;
                break;
            case PsTouchpadGesture::Tap:
                snapshot.gesture = app::TouchpadFeedbackGesture::Tap;
                break;
            case PsTouchpadGesture::Pan:
                snapshot.gesture = app::TouchpadFeedbackGesture::Pan;
                break;
            case PsTouchpadGesture::LongPress:
                snapshot.gesture = app::TouchpadFeedbackGesture::LongPress;
                break;
            case PsTouchpadGesture::None:
                break;
        }
        for (size_t i = 0; i < snapshot.points.size(); ++i) {
            snapshot.points[i] = {
                feedback.points[i].active,
                feedback.points[i].screen_x,
                feedback.points[i].screen_y,
            };
        }
        std::lock_guard<std::mutex> lock(touchpad_feedback_mutex_);
        touchpad_feedback_ = snapshot;
    }
    if (ps_button_requested_.exchange(false)) {
        ps_button_pulse_frames_remaining_ = kPsButtonPulseFrames;
        ps_button_release_pending_ = true;
    }
    if (ps_button_pulse_frames_remaining_ > 0) {
        // A one-frame PS press is easy for the console UI to miss. Hold the
        // virtual button briefly, then send an unambiguous release frame.
        state = {};
        state.guide = true;
        ps_button_pulse_frames_remaining_--;
    } else if (ps_button_release_pending_) {
        state = {};
        ps_button_release_pending_ = false;
    }
    state = input_router_.route(state);

    const PsMotionState motion = motion_reader_
        ? motion_reader_->read(input_suppressed)
        : PsMotionState{};
    ChiakiControllerState cs = input_mapper_->map(state, touchpad, &motion);
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
    if (rumble_) rumble_->update();
}

void PsStreamController::presentVideoFrame() {
    std::shared_lock<std::shared_mutex> operation_lock(stream_operation_mutex_);
    if (media_) media_->presentVideoFrame();
}

void PsStreamController::setState(app::StreamState s, const std::string& info) {
    state_.store(s);
    if ((s == app::StreamState::Error ||
         s == app::StreamState::Disconnected) && rumble_) {
        rumble_->stop();
    }
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
