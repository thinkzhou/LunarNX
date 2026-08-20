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
#include <deque>
#include <mutex>

namespace lunar::webrtc {

struct PeerManagerQueueTestAccess;

struct IceCandidate {
    std::string sdp;          // Full candidate line
    std::string sdp_mid;
    int sdp_mline_index = 0;
    std::string ufrag;        // ice-ufrag from SDP
    std::string mdns_name;    // optional mDNS hostname
};

struct PeerMediaStats : PeerConnectionMediaStats {
    uint32_t video_rtp_missing_packets_detected = 0;
    uint32_t video_rtp_missing_packets_recovered = 0;
    uint32_t video_rtp_nacks = 0;
    uint32_t video_rtp_nack_retries = 0;
    uint32_t video_rtp_resyncs = 0;
    uint32_t video_rtp_last_gap_packets = 0;
    uint32_t video_rtp_ssrc = 0;
    uint32_t video_rtp_ssrc_changes = 0;
    uint32_t video_rtp_arrival_age_ms = 0;
    uint32_t video_rtp_last_arrival_gap_ms = 0;
    uint32_t video_rtp_max_arrival_gap_ms = 0;
    uint32_t video_jitter_buffered_packets = 0;
    uint32_t video_jitter_buffered_frames = 0;
    uint32_t video_jitter_buffered_bytes = 0;
    bool video_waiting_keyframe = true;
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
    bool sendLatestInputData(const uint8_t* data, size_t len);
    bool sendControlData(const uint8_t* data, size_t len);
    bool sendMessageData(const uint8_t* data, size_t len);
    bool requestVideoKeyframe();
    bool sendReceiverFeedback(uint32_t bitrate_bps);
    bool hasPendingReliableData() const;
    bool consumeDataChannelFailure();
    bool isDataChannelReady() const;
    void setMediaEnabled(bool enabled);
    PeerMediaStats getMediaStats() const;

    // Connection
    bool isConnected() const;
    void processEvents();
    void disconnect();

private:
    friend struct PeerManagerQueueTestAccess;
    enum class OutboundType {
        InputReliable,
        InputLatest,
        Control,
        Message,
        Nack,
        Pli,
        ReceiverFeedback,
    };

    struct OutboundCommand {
        OutboundType type = OutboundType::InputReliable;
        std::vector<uint8_t> payload;
        uint16_t pid = 0;
        uint16_t blp = 0;
        uint8_t fraction_lost = 0;
        uint32_t cumulative_lost = 0;
        uint32_t highest_sequence = 0;
        uint32_t bitrate_bps = 0;
        uint64_t id = 0;
        uint32_t attempts = 0;
        std::chrono::steady_clock::time_point first_attempt_at{};
        std::chrono::steady_clock::time_point expires_at{};
    };

    static constexpr size_t kMaxOutboundCommands = 64;
    static constexpr size_t kMaxOutboundPayloadBytes = 1024;
    static constexpr uint8_t kMaxPliSendAttempts = 3;
    PeerConnection* pc_ = nullptr;
    PeerCallbacks callbacks_;
    std::atomic<bool> connected_{false};
    bool initialized_ = false;
    bool data_channels_created_ = false;
    std::atomic<int> video_callback_logs_{0};
    std::atomic<uint32_t> crash_probe_access_units_{0};
    std::atomic<int> audio_callback_logs_{0};
    std::atomic<int> process_event_logs_{0};
    std::atomic<int> rumble_parse_logs_{0};
    std::atomic<int> nack_logs_{0};
    uint64_t last_pump_socket_receive_us_total_ = 0;
    uint64_t last_pump_receive_loop_us_total_ = 0;
    uint64_t last_pump_rtp_drain_us_total_ = 0;
    uint64_t last_pump_socket_packets_total_ = 0;
    uint64_t last_pump_rtp_packets_decoded_total_ = 0;
    uint64_t max_pump_phase_total_us_ = 0;
    uint64_t slow_pump_count_ = 0;
    std::chrono::steady_clock::time_point last_pump_phase_log_{};

    // ICE candidate collection
    std::vector<IceCandidate> local_candidates_;
    std::chrono::steady_clock::time_point media_clock_start_;
    RtpClockMapper video_clock_{90000};
    RtpClockMapper audio_clock_{48000};
    VideoRtpJitterBuffer video_jitter_;
    mutable std::mutex outbound_mutex_;
    std::deque<OutboundCommand> outbound_commands_;
    uint64_t next_outbound_command_id_ = 1;
    uint32_t next_input_sequence_ = 0;
    std::atomic<bool> data_channel_failed_{false};
    std::atomic<bool> data_channel_failure_event_{false};
    uint32_t consecutive_sctp_send_failures_ = 0;
    std::chrono::steady_clock::time_point first_sctp_send_failure_{};
    std::atomic<uint32_t> outbound_drop_events_{0};

    bool enqueueData(OutboundType type,
                     const uint8_t* data,
                     size_t len,
                     bool replace_existing);
    bool enqueueNack(uint16_t pid, uint16_t blp);
    bool enqueueSimple(OutboundCommand command, bool high_priority);
    void drainOutboundCommands(std::chrono::steady_clock::time_point deadline);
    bool completeOutboundCommand(const OutboundCommand& command, int result);
    int sendOutboundCommand(const OutboundCommand& command);
    int sendInputCommand(const OutboundCommand& command);
    bool prepareSequencedInputPayload(const OutboundCommand& command,
                                      std::vector<uint8_t>& packet) const;
    void commitSequencedInputResult(int result);
    void observeSctpSendResult(OutboundType type,
                               int result,
                               uint32_t attempts);
    void markDataChannelFailed(const char* reason,
                               OutboundType type,
                               int result,
                               uint32_t attempts);
    void resetDataChannelHealth();
    void clearOutboundCommands();
    static bool isReliableCommand(OutboundType type);
    static bool isSctpCommand(OutboundType type);
    static int outboundPriority(OutboundType type);
    bool selectOutboundCommand(OutboundCommand& command,
                               bool allow_sctp) const;
    void logOutboundDrop(const char* reason,
                         OutboundType type,
                         int result = 0) noexcept;

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
