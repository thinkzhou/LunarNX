#ifdef __SWITCH__

#include "ps_stream_session.h"
#include "chiaki_log_adapter.h"
#include "../diagnostics.h"
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace lunar::ps {

namespace {

long long elapsedMs(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
}

} // namespace

void PsStreamSession::setLastError(std::string error) {
    std::lock_guard<std::mutex> lock(last_error_mutex_);
    last_error_ = std::move(error);
}

void PsStreamSession::releaseRemoteHolepunch() {
    if (!remote_result_.holepunch_session) return;
    chiaki_holepunch_session_fini(remote_result_.holepunch_session);
    remote_result_.holepunch_session = nullptr;
    remote_result_.valid = false;
}

bool PsStreamSession::startupCancelled(const char* stage) {
    if (!callbacks_.external_cancel || !callbacks_.external_cancel()) return false;
    setLastError(std::string("Session start cancelled at ") + stage);
    if (trace_) trace_->record(
        "session-cancel", "cancelled", "stage=%s", stage ? stage : "unknown");
    diagnosticLog("ps-session", "%s", lastError().c_str());
    state_.store(PsSessionState::Idle);
    return true;
}

PsStreamSession::PsStreamSession(const std::string& host_addr,
                                  const uint8_t* regist_key, const uint8_t* morning,
                                  int target,
                                  int width, int height, int fps, int bitrate_kbps,
                                  stream::VideoCodec video_codec,
                                  PsMediaBridge& bridge,
                                  std::shared_ptr<PsConnectionTrace> trace)
    : host_addr_(host_addr)
    , is_ps5_(target >= 1000000)
    , width_(width)
    , height_(height)
    , fps_(fps)
    , bitrate_kbps_(bitrate_kbps)
    , video_codec_(is_ps5_ ? video_codec : stream::VideoCodec::H264)
    , bridge_(bridge)
    , trace_(std::move(trace)) {
    std::memcpy(regist_key_, regist_key, sizeof(regist_key_));
    std::memcpy(morning_, morning, sizeof(morning_));
}

PsStreamSession::~PsStreamSession() {
    stop();
    bridge_.setEventForwarder({});
}

void PsStreamSession::configureConnectInfo() {
    connect_info_ = {};
    connect_info_.ps5 = is_ps5_;
    diagnosticLog("ps-session", "configure target_ps5=%d host=%s",
                  connect_info_.ps5 ? 1 : 0, host_addr_.c_str());
    if (trace_) trace_->record(
        "session-config", "begin",
        "mode=%s target=%s host_present=%d profile=%dx%d@%d bitrate_kbps=%d codec=%s",
        remote_mode_ ? "remote" : "lan", is_ps5_ ? "ps5" : "ps4",
        host_addr_.empty() ? 0 : 1, width_, height_, fps_, bitrate_kbps_,
        video_codec_ == stream::VideoCodec::HEVC ? "h265" : "h264");
    connect_info_.video_profile.width = static_cast<unsigned int>(width_);
    connect_info_.video_profile.height = static_cast<unsigned int>(height_);
    connect_info_.video_profile.max_fps = static_cast<unsigned int>(fps_);
    connect_info_.video_profile.bitrate = static_cast<unsigned int>(bitrate_kbps_);
    connect_info_.video_profile.codec = video_codec_ == stream::VideoCodec::HEVC
        ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
    connect_info_.video_profile_auto_downgrade = true;
    connect_info_.packet_loss_max = 0.05;
    connect_info_.enable_dualsense = is_ps5_;
    connect_info_.enable_idr_on_fec_failure = true;
    // LunarNX does not expose Chiaki's remote keyboard protocol. Keep this
    // disabled like chiaki-ng instead of advertising an unsupported optional
    // feature and sending extra CTRL messages after the session ID arrives.
    connect_info_.enable_keyboard = false;
    std::memcpy(connect_info_.regist_key, regist_key_, sizeof(regist_key_));
    std::memcpy(connect_info_.morning, morning_, sizeof(morning_));

    if (remote_mode_ && remote_result_.valid) {
        connect_info_.host = host_addr_.c_str();
        connect_info_.holepunch_session = remote_result_.holepunch_session;
        if (remote_result_.psn_account_id.size() !=
            sizeof(connect_info_.psn_account_id)) {
            if (trace_) trace_->record(
                "session-config", "failed",
                "reason=account-size actual=%zu expected=%zu",
                remote_result_.psn_account_id.size(),
                sizeof(connect_info_.psn_account_id));
            setLastError("PSN account ID has invalid size");
            return;
        }
        std::memcpy(connect_info_.psn_account_id,
                    remote_result_.psn_account_id.data(),
                    sizeof(connect_info_.psn_account_id));
    } else {
        connect_info_.host = host_addr_.c_str();
    }
    if (trace_) trace_->record(
        "session-config", "ok",
        "remote_holepunch=%d auto_downgrade=1 dualsense=%d keyboard=0 idr_on_fec_failure=1 packet_loss_max=0.05",
        connect_info_.holepunch_session ? 1 : 0,
        connect_info_.enable_dualsense ? 1 : 0);
}

bool PsStreamSession::doStart(PsSessionCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
    state_.store(PsSessionState::Connecting);

    if (startupCancelled("before init")) {
        releaseRemoteHolepunch();
        return false;
    }

    log_ = makeChiakiDiagnosticLog("chiaki-session");
    if (trace_) trace_->record("audio-decoder", "begin", "codec=opus");
    bridge_.initializeAudio(&log_);
    if (trace_) trace_->record("audio-decoder", "configured", "codec=opus");
    diagnosticLog("ps-session", "doStart begin remote_mode=%d valid=%d acct_size=%zu BUILD_V3_SESSION_INIT_LOG",
                  remote_mode_, remote_result_.valid,
                  remote_result_.psn_account_id.size());
    setLastError({});
    configureConnectInfo();
    const std::string configure_error = lastError();
    if (!configure_error.empty()) {
        state_.store(PsSessionState::Error);
        releaseRemoteHolepunch();
        if (callbacks_.on_error) callbacks_.on_error(configure_error);
        return false;
    }

    if (startupCancelled("after configure")) {
        releaseRemoteHolepunch();
        return false;
    }

    diagnosticLog("ps-session", "calling chiaki_session_init session=%p connect_info=%p log=%p log_mask=0x%x log_cb=%p log_user=%p",
                  static_cast<void*>(&session_), static_cast<void*>(&connect_info_),
                  static_cast<void*>(&log_), log_.level_mask,
                  reinterpret_cast<void*>(log_.cb), log_.user);
    const auto init_started = std::chrono::steady_clock::now();
    ChiakiErrorCode err = chiaki_session_init(&session_, &connect_info_, &log_);
    diagnosticLog("ps-session", "chiaki_session_init rc=%d", err);
    if (remote_mode_) {
        remote_result_.holepunch_session = nullptr;
        remote_result_.valid = false;
    }
    if (trace_) trace_->record(
        "chiaki-init", err == CHIAKI_ERR_SUCCESS ? "ok" : "failed",
        "mode=%s elapsed_ms=%lld error=%d error_name=%s",
        remote_mode_ ? "remote" : "lan", elapsedMs(init_started),
        static_cast<int>(err), chiaki_error_string(err));
    if (err != CHIAKI_ERR_SUCCESS) {
        setLastError("Session init failed: " + std::string(chiaki_error_string(err)));
        state_.store(PsSessionState::Error);
        if (callbacks_.on_error) callbacks_.on_error(lastError());
        return false;
    }
    initialized_ = true;

    // Route every Chiaki event through the media bridge. It forwards normal
    // lifecycle events back to this session and handles rumble before doing
    // so; registering sessionEventCb directly would silently bypass rumble.
    bridge_.setEventForwarder([this](ChiakiEvent* event) {
        handleEvent(event);
    });
    chiaki_session_set_event_cb(
        &session_, bridge_.eventCallback(), &bridge_);
    chiaki_session_set_video_sample_cb(&session_, videoSampleCb, this);

    ChiakiAudioSink sink = bridge_.audioSink();
    chiaki_session_set_audio_sink(&session_, &sink);
    if (connect_info_.enable_dualsense) {
        ChiakiAudioSink haptics_sink = bridge_.hapticsSink();
        chiaki_session_set_haptics_sink(&session_, &haptics_sink);
    }
    if (trace_) trace_->record(
        "session-callbacks", "configured",
        "events=1 video=1 audio=1 haptics=%d rumble-via-bridge=1",
        connect_info_.enable_dualsense ? 1 : 0);

    if (startupCancelled("after init")) {
        chiaki_session_fini(&session_);
        initialized_ = false;
        return false;
    }

    diagnosticLog("ps-session", "calling chiaki_session_start");
    const auto start_started = std::chrono::steady_clock::now();
    err = chiaki_session_start(&session_);
    diagnosticLog("ps-session", "chiaki_session_start rc=%d", err);
    if (err != CHIAKI_ERR_SUCCESS) {
        if (trace_) trace_->record(
            "chiaki-start", "failed", "elapsed_ms=%lld error=%d error_name=%s",
            elapsedMs(start_started), static_cast<int>(err),
            chiaki_error_string(err));
        setLastError("Session start failed: " + std::string(chiaki_error_string(err)));
        chiaki_session_fini(&session_);
        initialized_ = false;
        state_.store(PsSessionState::Error);
        if (callbacks_.on_error) callbacks_.on_error(lastError());
        return false;
    }
    if (trace_) trace_->record(
        "chiaki-start", "ok", "mode=%s elapsed_ms=%lld",
        remote_mode_ ? "remote" : "lan", elapsedMs(start_started));
    started_ = true;

    if (startupCancelled("after start")) {
        stop();
        return false;
    }

    const char* mode = remote_mode_ ? "remote" : "LAN";
    diagnosticLog("ps-session", "Session started (%s) for %s",
                  mode, remote_mode_ ? "PSN" : host_addr_.c_str());
    if (trace_) trace_->record(
        "session-thread", "running", "mode=%s", mode);
    return true;
}

bool PsStreamSession::start(PsSessionCallbacks callbacks) {
    remote_mode_ = false;
    return doStart(std::move(callbacks));
}

bool PsStreamSession::startRemote(PsRemoteResult&& remote, PsSessionCallbacks callbacks) {
    remote_mode_ = true;
    successful_remote_route_ = {};
    remote_result_ = std::move(remote);
    remote = {};
    return doStart(std::move(callbacks));
}

PsRoutePreference PsStreamSession::successfulRemoteRoute() const {
    return successful_remote_route_;
}

void PsStreamSession::stop() {
    if (state_.load() == PsSessionState::Stopping) return;
    const auto stop_started = std::chrono::steady_clock::now();
    state_.store(PsSessionState::Stopping);
    lunar::persistentEventLog(
        "ps-session-stop", "stop begin started=%d initialized=%d",
        started_ ? 1 : 0, initialized_ ? 1 : 0);
    if (trace_ && !trace_->finished()) trace_->record(
        "session-stop", "begin", "started=%d initialized=%d",
        started_ ? 1 : 0, initialized_ ? 1 : 0);

    auto phase_started = std::chrono::steady_clock::now();
    lunar::persistentEventLog(
        "ps-session-stop", "phase=chiaki-stop begin active=%d",
        started_ ? 1 : 0);
    if (started_) {
        chiaki_session_stop(&session_);
    }
    lunar::persistentEventLog(
        "ps-session-stop", "phase=chiaki-stop done elapsed_ms=%lld",
        elapsedMs(phase_started));

    phase_started = std::chrono::steady_clock::now();
    lunar::persistentEventLog(
        "ps-session-stop", "phase=chiaki-join begin active=%d",
        started_ ? 1 : 0);
    if (started_) {
        chiaki_session_join(&session_);
        started_ = false;
    }
    lunar::persistentEventLog(
        "ps-session-stop", "phase=chiaki-join done elapsed_ms=%lld",
        elapsedMs(phase_started));

    phase_started = std::chrono::steady_clock::now();
    lunar::persistentEventLog(
        "ps-session-stop", "phase=chiaki-fini begin active=%d",
        initialized_ ? 1 : 0);
    if (initialized_) {
        chiaki_session_fini(&session_);
        initialized_ = false;
    }
    lunar::persistentEventLog(
        "ps-session-stop", "phase=chiaki-fini done elapsed_ms=%lld",
        elapsedMs(phase_started));

    remote_mode_ = false;
    state_.store(PsSessionState::Idle);
    lunar::persistentEventLog(
        "ps-session-stop", "stop complete total_ms=%lld",
        elapsedMs(stop_started));
}

// ... rest (setLoginPin, setControllerState, requestIDR, event handling) unchanged ...

void PsStreamSession::setLoginPin(const std::string& pin) {
    if (pin.empty()) return;
    chiaki_session_set_login_pin(&session_,
        reinterpret_cast<const uint8_t*>(pin.c_str()), pin.size());
}

void PsStreamSession::setControllerState(ChiakiControllerState& state) {
    if (!started_) return;
    chiaki_session_set_controller_state(&session_, &state);
}

void PsStreamSession::requestIDR() {
    if (!started_) return;
    chiaki_session_request_idr(&session_);
}

PsTransportStats PsStreamSession::transportStats() const {
    PsTransportStats stats;
    stats.rtt_ms = transport_rtt_ms_.load(std::memory_order_relaxed);
    stats.measured_bitrate_mbps = transport_bitrate_mbps_.load(std::memory_order_relaxed);
    stats.packet_loss_fraction = transport_packet_loss_.load(std::memory_order_relaxed);
    stats.frames_lost = transport_frames_lost_.load(std::memory_order_relaxed);
    return stats;
}

void PsStreamSession::refreshTransportStats() {
    if (!initialized_) return;
    const double dynamic_rtt = session_.stream_connection.rtt;
    const uint32_t rtt_ms = dynamic_rtt > 0.0
        ? static_cast<uint32_t>(std::lround(dynamic_rtt))
        : static_cast<uint32_t>(session_.rtt_us / 1000ULL);
    transport_rtt_ms_.store(rtt_ms, std::memory_order_relaxed);
    transport_bitrate_mbps_.store(static_cast<float>(
        session_.stream_connection.measured_bitrate), std::memory_order_relaxed);
    auto* congestion = &session_.stream_connection.congestion_control;
    if (chiaki_bool_pred_cond_lock(&congestion->stop_cond) == CHIAKI_ERR_SUCCESS) {
        transport_packet_loss_.store(static_cast<float>(congestion->packet_loss),
                                     std::memory_order_relaxed);
        chiaki_bool_pred_cond_unlock(&congestion->stop_cond);
    }
    if (session_.stream_connection.video_receiver) {
        transport_frames_lost_.store(static_cast<uint32_t>(std::max(
            chiaki_video_receiver_get_frames_lost_total(
                session_.stream_connection.video_receiver), 0)), std::memory_order_relaxed);
    }
}

void PsStreamSession::maybeRefreshTransportStats() {
    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    uint64_t last_ms = transport_refresh_ms_.load(std::memory_order_relaxed);
    if (now_ms - last_ms >= 500 &&
        transport_refresh_ms_.compare_exchange_strong(
            last_ms, now_ms, std::memory_order_relaxed)) {
        refreshTransportStats();
    }
}

bool PsStreamSession::videoSampleCb(uint8_t* buf, size_t buf_size,
                                    int32_t frames_lost, bool frame_recovered,
                                    void* user) {
    auto* self = static_cast<PsStreamSession*>(user);
    if (!self) return false;
    self->maybeRefreshTransportStats();
    return self->bridge_.onVideoSample(buf, buf_size, frames_lost, frame_recovered);
}

void PsStreamSession::handleEvent(ChiakiEvent* event) {
    diagnosticLog("ps-event", "type=%d", static_cast<int>(event->type));
    switch (event->type) {
        case CHIAKI_EVENT_CONNECTED: {
            if (remote_mode_ && session_.holepunch_session) {
                char stun_host[254] = {};
                uint16_t stun_port = 0;
                if (chiaki_holepunch_session_get_successful_stun_server(
                        session_.holepunch_session, stun_host, sizeof(stun_host),
                        &stun_port)) {
                    successful_remote_route_.preferred_stun_host = stun_host;
                    successful_remote_route_.preferred_stun_port = stun_port;
                }
                char remote_address[INET6_ADDRSTRLEN] = {};
                uint16_t remote_port = 0;
                if (chiaki_holepunch_session_get_selected_remote_candidate(
                        session_.holepunch_session, remote_address,
                        sizeof(remote_address), &remote_port)) {
                    successful_remote_route_.remote_address = remote_address;
                    successful_remote_route_.remote_port = remote_port;
                }
            }
            diagnosticLog("ps-session", "Connected");
            if (trace_) trace_->record(
                "connected", "ok",
                "ctrl_failed=%d ctrl_session_id=%d stream_switch=%d",
                session_.ctrl_failed ? 1 : 0,
                session_.ctrl_session_id_received ? 1 : 0,
                session_.stream_connection_switch_received ? 1 : 0);
            state_.store(PsSessionState::Streaming);
            if (callbacks_.on_streaming) callbacks_.on_streaming();
            break;
        }
        case CHIAKI_EVENT_LOGIN_PIN_REQUEST: {
            if (trace_) trace_->record(
                "login-pin", "required", "incorrect=%d",
                event->login_pin_request.pin_incorrect ? 1 : 0);
            if (callbacks_.on_status) {
                callbacks_.on_status(event->login_pin_request.pin_incorrect
                    ? "Console login PIN incorrect"
                    : "Console login PIN required");
            }
            if (callbacks_.on_login_pin_requested) {
                callbacks_.on_login_pin_requested(
                    event->login_pin_request.pin_incorrect);
            }
            break;
        }
        case CHIAKI_EVENT_HOLEPUNCH: {
            if (trace_) trace_->record(
                "data-hole",
                event->data_holepunch.finished ? "ready" : "begin",
                "finished=%d",
                event->data_holepunch.finished ? 1 : 0);
            if (callbacks_.on_status) {
                callbacks_.on_status(event->data_holepunch.finished
                    ? "Data channel established"
                    : "Punching data channel...");
            }
            break;
        }
        case CHIAKI_EVENT_REGIST: {
            if (trace_) trace_->record(
                "dynamic-register", "complete", "remote_mode=%d",
                remote_mode_ ? 1 : 0);
            if (callbacks_.on_registered) callbacks_.on_registered(event->host);
            break;
        }
        case CHIAKI_EVENT_QUIT: {
            const char* reason_str = event->quit.reason_str
                ? event->quit.reason_str : "unknown";
            diagnosticLog("ps-session",
                "Quit: reason=%d reason_str=%s ctrl_failed=%d ctrl_session_id=%d stream_switch=%d",
                static_cast<int>(event->quit.reason),
                reason_str,
                session_.ctrl_failed ? 1 : 0,
                session_.ctrl_session_id_received ? 1 : 0,
                session_.stream_connection_switch_received ? 1 : 0);
            if (trace_) trace_->record(
                "quit",
                event->quit.reason == CHIAKI_QUIT_REASON_STOPPED
                    ? "stopped" : chiaki_quit_reason_is_error(event->quit.reason)
                        ? "error" : "disconnected",
                "reason=%d reason_text=%s ctrl_failed=%d ctrl_session_id=%d stream_switch=%d",
                static_cast<int>(event->quit.reason), reason_str,
                session_.ctrl_failed ? 1 : 0,
                session_.ctrl_session_id_received ? 1 : 0,
                session_.stream_connection_switch_received ? 1 : 0);
            if (event->quit.reason == CHIAKI_QUIT_REASON_STOPPED) {
                if (callbacks_.on_disconnected) callbacks_.on_disconnected(reason_str);
            } else if (chiaki_quit_reason_is_error(event->quit.reason)) {
                setLastError("Session error: " + std::string(reason_str));
                state_.store(PsSessionState::Error);
                if (callbacks_.on_error) callbacks_.on_error(lastError());
            } else {
                if (callbacks_.on_disconnected) callbacks_.on_disconnected(reason_str);
            }
            break;
        }
        default:
            diagnosticLog("ps-session", "Event: type=%d", static_cast<int>(event->type));
            break;
    }
}

} // namespace lunar::ps

#endif
