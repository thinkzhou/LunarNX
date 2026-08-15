#pragma once

#ifdef __SWITCH__

#include "ps_media_bridge.h"
#include "ps_remote_connector.h"
#include <netinet/in.h>
#include <chiaki/session.h>
#include <chiaki/remote/holepunch.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace lunar::ps {

enum class PsSessionState {
    Idle,
    Connecting,
    Streaming,
    Stopping,
    Error,
};

struct PsTransportStats {
    uint32_t rtt_ms = 0;
    float measured_bitrate_mbps = 0.0f;
    float packet_loss_fraction = 0.0f;
    uint32_t frames_lost = 0;
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
                    stream::VideoCodec video_codec,
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
    std::string lastError() const {
        std::lock_guard<std::mutex> lock(last_error_mutex_);
        return last_error_;
    }

    void setLoginPin(const std::string& pin);
    void setControllerState(ChiakiControllerState& state);
    void requestIDR();
    PsTransportStats transportStats() const;

private:
    void configureConnectInfo();
    bool doStart(PsSessionCallbacks callbacks);
    bool startupCancelled(const char* stage);
    void releaseRemoteHolepunch();
    static bool videoSampleCb(uint8_t* buf, size_t buf_size, int32_t frames_lost,
                              bool frame_recovered, void* user);
    void maybeRefreshTransportStats();
    void refreshTransportStats();
    void setLastError(std::string error);
    void handleEvent(ChiakiEvent* event);

    std::string host_addr_;
    uint8_t regist_key_[0x10];
    uint8_t morning_[0x10];
    bool is_ps5_;
    int width_, height_, fps_, bitrate_kbps_;
    stream::VideoCodec video_codec_;
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
    std::atomic<uint32_t> transport_rtt_ms_{0};
    std::atomic<float> transport_bitrate_mbps_{0.0f};
    std::atomic<float> transport_packet_loss_{0.0f};
    std::atomic<uint32_t> transport_frames_lost_{0};
    std::atomic<uint64_t> transport_refresh_ms_{0};
    std::string last_error_;
    mutable std::mutex last_error_mutex_;
    PsSessionCallbacks callbacks_;
};

} // namespace lunar::ps

#endif
