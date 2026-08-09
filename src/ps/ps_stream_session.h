#pragma once

#ifdef __SWITCH__

#include "ps_media_bridge.h"
#include "ps_remote_connector.h"
#include <netinet/in.h>
#include <chiaki/session.h>
#include <chiaki/remote/holepunch.h>
#include <atomic>
#include <functional>
#include <string>

namespace lunar::ps {

enum class PsSessionState {
    Idle,
    Connecting,
    Streaming,
    Stopping,
    Error,
};

struct PsSessionCallbacks {
    std::function<void(const std::string&)> on_status;
    std::function<void()> on_streaming;
    std::function<void(bool incorrect)> on_login_pin_requested;
    std::function<void(const ChiakiRegisteredHost&)> on_registered;
    std::function<void(const std::string&)> on_error;
    std::function<void(const std::string&)> on_disconnected;
    std::function<bool()> external_cancel;
};

class PsStreamSession {
public:
    PsStreamSession(const std::string& host_addr,
                    const uint8_t* regist_key, const uint8_t* morning,
                    int target,
                    int width, int height, int fps, int bitrate_kbps,
                    PsMediaBridge& bridge);
    ~PsStreamSession();

    PsStreamSession(const PsStreamSession&) = delete;
    PsStreamSession& operator=(const PsStreamSession&) = delete;

    // LAN mode: connect directly to host
    bool start(PsSessionCallbacks callbacks);

    // Remote mode: Chiaki owns dynamic registration and data hole punching.
    bool startRemote(PsRemoteResult&& remote, PsSessionCallbacks callbacks);

    void stop();
    PsSessionState state() const { return state_.load(); }
    std::string lastError() const { return last_error_; }

    void setLoginPin(const std::string& pin);
    void setControllerState(ChiakiControllerState& state);
    void requestIDR();

private:
    void configureConnectInfo();
    bool doStart(PsSessionCallbacks callbacks);
    bool startupCancelled(const char* stage);
    void releaseRemoteHolepunch();
    static void sessionEventCb(ChiakiEvent* event, void* user);
    void handleEvent(ChiakiEvent* event);

    std::string host_addr_;
    uint8_t regist_key_[0x10];
    uint8_t morning_[0x10];
    bool is_ps5_;
    int width_, height_, fps_, bitrate_kbps_;
    PsMediaBridge& bridge_;

    // Remote mode
    bool remote_mode_ = false;
    PsRemoteResult remote_result_;

    ChiakiLog log_{};
    ChiakiSession session_{};
    ChiakiConnectInfo connect_info_{};
    bool initialized_ = false;
    bool started_ = false;

    std::atomic<PsSessionState> state_{PsSessionState::Idle};
    std::string last_error_;
    PsSessionCallbacks callbacks_;
};

} // namespace lunar::ps

#endif
