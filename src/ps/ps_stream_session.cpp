#ifdef __SWITCH__

#include "ps_stream_session.h"
#include "chiaki_log_adapter.h"
#include "../diagnostics.h"
#include <cstring>
#include <utility>

namespace lunar::ps {

void PsStreamSession::releaseRemoteHolepunch() {
    if (!remote_result_.holepunch_session) return;
    chiaki_holepunch_session_fini(remote_result_.holepunch_session);
    remote_result_.holepunch_session = nullptr;
    remote_result_.valid = false;
}

bool PsStreamSession::startupCancelled(const char* stage) {
    if (!callbacks_.external_cancel || !callbacks_.external_cancel()) return false;
    last_error_ = std::string("Session start cancelled at ") + stage;
    diagnosticLog("ps-session", "%s", last_error_.c_str());
    state_.store(PsSessionState::Idle);
    return true;
}

PsStreamSession::PsStreamSession(const std::string& host_addr,
                                  const uint8_t* regist_key, const uint8_t* morning,
                                  int target,
                                  int width, int height, int fps, int bitrate_kbps,
                                  PsMediaBridge& bridge)
    : host_addr_(host_addr)
    , is_ps5_(target >= 1000000)
    , width_(width)
    , height_(height)
    , fps_(fps)
    , bitrate_kbps_(bitrate_kbps)
    , bridge_(bridge) {
    std::memcpy(regist_key_, regist_key, sizeof(regist_key_));
    std::memcpy(morning_, morning, sizeof(morning_));
}

PsStreamSession::~PsStreamSession() {
    stop();
}

void PsStreamSession::configureConnectInfo() {
    connect_info_ = {};
    connect_info_.ps5 = is_ps5_;
    diagnosticLog("ps-session", "configure target_ps5=%d host=%s",
                  connect_info_.ps5 ? 1 : 0, host_addr_.c_str());
    connect_info_.video_profile.width = static_cast<unsigned int>(width_);
    connect_info_.video_profile.height = static_cast<unsigned int>(height_);
    connect_info_.video_profile.max_fps = static_cast<unsigned int>(fps_);
    connect_info_.video_profile.bitrate = static_cast<unsigned int>(bitrate_kbps_);
    connect_info_.video_profile.codec = CHIAKI_CODEC_H264;
    connect_info_.video_profile_auto_downgrade = true;
    connect_info_.packet_loss_max = 0.02;
    connect_info_.enable_dualsense = true;
    connect_info_.enable_keyboard = true;
    std::memcpy(connect_info_.regist_key, regist_key_, sizeof(regist_key_));
    std::memcpy(connect_info_.morning, morning_, sizeof(morning_));

    if (remote_mode_ && remote_result_.valid) {
        connect_info_.host = host_addr_.c_str();
        connect_info_.holepunch_session = remote_result_.holepunch_session;
        if (remote_result_.psn_account_id.size() !=
            sizeof(connect_info_.psn_account_id)) {
            last_error_ = "PSN account ID has invalid size";
            return;
        }
        std::memcpy(connect_info_.psn_account_id,
                    remote_result_.psn_account_id.data(),
                    sizeof(connect_info_.psn_account_id));
    } else {
        connect_info_.host = host_addr_.c_str();
    }
}

bool PsStreamSession::doStart(PsSessionCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
    state_.store(PsSessionState::Connecting);

    if (startupCancelled("before init")) {
        releaseRemoteHolepunch();
        return false;
    }

    log_ = makeChiakiDiagnosticLog("chiaki-session");
    bridge_.initializeAudio(&log_);
    diagnosticLog("ps-session", "doStart begin remote_mode=%d valid=%d acct_size=%zu BUILD_V3_SESSION_INIT_LOG",
                  remote_mode_, remote_result_.valid,
                  remote_result_.psn_account_id.size());
    last_error_.clear();
    configureConnectInfo();
    if (!last_error_.empty()) {
        state_.store(PsSessionState::Error);
        releaseRemoteHolepunch();
        if (callbacks_.on_error) callbacks_.on_error(last_error_);
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
    ChiakiErrorCode err = chiaki_session_init(&session_, &connect_info_, &log_);
    diagnosticLog("ps-session", "chiaki_session_init rc=%d", err);
    if (remote_mode_) {
        remote_result_.holepunch_session = nullptr;
        remote_result_.valid = false;
    }
    if (err != CHIAKI_ERR_SUCCESS) {
        last_error_ = "Session init failed: " + std::string(chiaki_error_string(err));
        state_.store(PsSessionState::Error);
        if (callbacks_.on_error) callbacks_.on_error(last_error_);
        return false;
    }
    initialized_ = true;

    chiaki_session_set_event_cb(&session_, sessionEventCb, this);
    chiaki_session_set_video_sample_cb(&session_, PsMediaBridge::videoSampleCb, &bridge_);

    ChiakiAudioSink sink = bridge_.audioSink();
    chiaki_session_set_audio_sink(&session_, &sink);

    if (startupCancelled("after init")) {
        chiaki_session_fini(&session_);
        initialized_ = false;
        return false;
    }

    diagnosticLog("ps-session", "calling chiaki_session_start");
    err = chiaki_session_start(&session_);
    diagnosticLog("ps-session", "chiaki_session_start rc=%d", err);
    if (err != CHIAKI_ERR_SUCCESS) {
        last_error_ = "Session start failed: " + std::string(chiaki_error_string(err));
        state_.store(PsSessionState::Error);
        if (callbacks_.on_error) callbacks_.on_error(last_error_);
        return false;
    }
    started_ = true;

    if (startupCancelled("after start")) {
        stop();
        return false;
    }

    const char* mode = remote_mode_ ? "remote" : "LAN";
    diagnosticLog("ps-session", "Session started (%s) for %s",
                  mode, remote_mode_ ? "PSN" : host_addr_.c_str());
    return true;
}

bool PsStreamSession::start(PsSessionCallbacks callbacks) {
    remote_mode_ = false;
    return doStart(std::move(callbacks));
}

bool PsStreamSession::startRemote(PsRemoteResult&& remote, PsSessionCallbacks callbacks) {
    remote_mode_ = true;
    remote_result_ = std::move(remote);
    remote = {};
    return doStart(std::move(callbacks));
}

void PsStreamSession::stop() {
    if (state_.load() == PsSessionState::Stopping) return;
    state_.store(PsSessionState::Stopping);

    if (started_) {
        chiaki_session_stop(&session_);
        chiaki_session_join(&session_);
        started_ = false;
    }
    if (initialized_) {
        chiaki_session_fini(&session_);
        initialized_ = false;
    }

    remote_mode_ = false;
    state_.store(PsSessionState::Idle);
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

void PsStreamSession::sessionEventCb(ChiakiEvent* event, void* user) {
    auto* self = static_cast<PsStreamSession*>(user);
    if (!self || !event) return;
    self->handleEvent(event);
}

void PsStreamSession::handleEvent(ChiakiEvent* event) {
    diagnosticLog("ps-event", "type=%d", static_cast<int>(event->type));
    switch (event->type) {
        case CHIAKI_EVENT_CONNECTED: {
            diagnosticLog("ps-session", "Connected");
            state_.store(PsSessionState::Streaming);
            if (callbacks_.on_streaming) callbacks_.on_streaming();
            break;
        }
        case CHIAKI_EVENT_LOGIN_PIN_REQUEST: {
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
            if (callbacks_.on_status) {
                callbacks_.on_status(event->data_holepunch.finished
                    ? "Data channel established"
                    : "Punching data channel...");
            }
            break;
        }
        case CHIAKI_EVENT_REGIST: {
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
            if (event->quit.reason == CHIAKI_QUIT_REASON_STOPPED) {
                if (callbacks_.on_disconnected) callbacks_.on_disconnected(reason_str);
            } else if (chiaki_quit_reason_is_error(event->quit.reason)) {
                last_error_ = "Session error: " + std::string(reason_str);
                state_.store(PsSessionState::Error);
                if (callbacks_.on_error) callbacks_.on_error(last_error_);
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
