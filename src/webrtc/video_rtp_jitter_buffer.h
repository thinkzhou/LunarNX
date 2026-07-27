#pragma once

#include "../diagnostics.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace lunar::webrtc {

struct VideoRtpReceiverReport {
    uint8_t fraction_lost = 0;
    uint32_t cumulative_lost = 0;
    uint32_t highest_sequence = 0;
};

struct VideoRtpJitterStats {
    uint32_t packets = 0;
    uint32_t sequence_gaps = 0;
    uint32_t missing_packets = 0;
    uint32_t frames = 0;
    uint32_t corrupt_frames = 0;
    uint32_t unsupported_nalus = 0;
    uint32_t overflow_frames = 0;
    uint32_t max_frame_bytes = 0;
    uint32_t nacks = 0;
    uint32_t resyncs = 0;
    uint32_t assembly_attempts = 0;
    uint32_t payload_storage_reallocations = 0;
    size_t buffered_bytes = 0;
    size_t buffered_packets = 0;
    size_t buffered_frames = 0;
    uint32_t highest_sequence = 0;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    uint32_t last_gap_packets = 0;
    uint32_t ssrc = 0;
    uint32_t ssrc_changes = 0;
    uint64_t last_arrival_ms = 0;
    uint64_t last_arrival_gap_ms = 0;
    uint64_t max_arrival_gap_ms = 0;
#endif
};

class VideoRtpJitterBuffer {
public:
    using EmitCallback =
        std::function<void(const uint8_t*, size_t, uint16_t, uint32_t)>;
    using NackCallback = std::function<bool(uint16_t, uint16_t)>;
    using RecoveryCallback = std::function<void(bool)>;

    static constexpr uint64_t kDefaultHoldMs = 120;
    static constexpr uint64_t kMaxFrameHoldMs = 250;
    static constexpr size_t kMaxBufferedFrames = 32;
    static constexpr size_t kMaxBufferedPackets = 2048;
    static constexpr size_t kMaxBufferedBytes = 3 * 1024 * 1024;
    static constexpr size_t kMaxAccessUnitBytes = 2 * 1024 * 1024;

    VideoRtpJitterBuffer();
    ~VideoRtpJitterBuffer();
    VideoRtpJitterBuffer(const VideoRtpJitterBuffer&) = delete;
    VideoRtpJitterBuffer& operator=(const VideoRtpJitterBuffer&) = delete;

    void reset();
    void setHoldMs(uint64_t hold_ms);
    void receive(const uint8_t* packet,
                 size_t size,
                 uint64_t now_ms,
                 const EmitCallback& emit,
                 const NackCallback& nack,
                 const RecoveryCallback& recovery);

    VideoRtpJitterStats stats() const;
    VideoRtpReceiverReport receiverReport();
    bool waitingForKeyframe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lunar::webrtc
