#pragma once

#include "../common.h"
#include "rtp_clock_mapper.h"
#include "video_rtp_jitter_buffer.h"
#include <peer_connection.h>
#include <string>
#include <functional>
#include <vector>
#include <chrono>
#include <atomic>
#include <cstdint>

namespace lunar::webrtc {

struct IceCandidate {
    std::string sdp;          // Full candidate line
    std::string sdp_mid;
    int sdp_mline_index = 0;
    std::string ufrag;        // ice-ufrag from SDP
    std::string mdns_name;    // optional mDNS hostname
};

struct PeerCallbacks {
    using MediaFrameCallback =
        std::function<void(const uint8_t*, size_t, uint16_t, uint64_t)>;

    MediaFrameCallback on_video_frame;
    std::function<void(size_t)> on_video_packet;
    MediaFrameCallback on_audio_frame;
    std::function<void(const std::string& label, const uint8_t* data, size_t len)> on_datachannel_message;
    std::function<void(const std::string& error)> on_error;
    // True marks the first transition into keyframe recovery. The media layer
    // keeps fenced/displayed frames alive and asks the sender for a fresh IDR;
    // decoder flushes remain reserved for actual decoder/queue failures.
    std::function<void(bool reset_decoder)> on_video_recovery;

    // Xbox 4-motor rumble. Called when Xbox sends vibration data.
    // Parameters match XStreaming: {leftMotor, rightMotor, leftTrigger, rightTrigger} 0.0-1.0
    // durationMs: how long to vibrate, delayMs: start delay, repeat: 0=once
    std::function<void(uint8_t gamepadIndex,
                       float left, float right, float lt, float rt,
                       uint16_t durMs, uint16_t delayMs, uint8_t repeat)> on_rumble;
};

class PeerManager {
public:
    PeerManager();
    ~PeerManager();

    bool initialize();
    void setCallbacks(const PeerCallbacks& callbacks);

    // SDP
    std::string createOffer();
    void setRemoteAnswer(const std::string& sdp);

    // ICE
    std::vector<IceCandidate> getLocalCandidates();
    void clearLocalCandidates() { local_candidates_.clear(); }
    void addIceCandidate(const std::string& candidate_sdp);

    // Locally initiated DTLS-client channels use XStreaming's even SID order.
    bool createDataChannels();
    bool sendInputData(const uint8_t* data, size_t len);
    bool sendControlData(const uint8_t* data, size_t len);
    bool sendMessageData(const uint8_t* data, size_t len);
    bool requestVideoKeyframe();
    bool sendReceiverFeedback(uint32_t bitrate_bps);
    bool isDataChannelReady() const;
    void setMediaEnabled(bool enabled);
    PeerConnectionMediaStats getMediaStats() const;

    // Connection
    bool isConnected() const;
    void processEvents();
    void disconnect();

private:
    PeerConnection* pc_ = nullptr;
    PeerCallbacks callbacks_;
    std::atomic<bool> connected_{false};
    bool initialized_ = false;
    bool data_channels_created_ = false;
    std::atomic<int> video_callback_logs_{0};
    std::atomic<int> audio_callback_logs_{0};
    std::atomic<int> process_event_logs_{0};
    std::atomic<int> rumble_parse_logs_{0};
    std::atomic<int> nack_logs_{0};

    // ICE candidate collection
    std::vector<IceCandidate> local_candidates_;
    std::chrono::steady_clock::time_point media_clock_start_;
    RtpClockMapper video_clock_{90000};
    RtpClockMapper audio_clock_{48000};
    VideoRtpJitterBuffer video_jitter_;

    static void onIceCandidate(char* sdp_text, void* userdata);
    static void onIceStateChange(PeerConnectionState state, void* userdata);
    static void onVideoTrack(uint8_t* data,
                             size_t size,
                             uint16_t sequence,
                             uint32_t timestamp,
                             void* userdata);
    static void onAudioTrack(uint8_t* data,
                             size_t size,
                             uint16_t sequence,
                             uint32_t timestamp,
                             void* userdata);
    void handleVideoJitterRecovery(bool reset_decoder) noexcept;
    static void onDataChannelMessage(char* msg, size_t len, void* userdata, uint16_t sid);
};

} // namespace lunar::webrtc
