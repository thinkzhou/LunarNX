#pragma once

#include <cstddef>
#include <cstdint>

namespace lunar::stream {

// BufferedFifo preserves one decoded frame of elasticity. RealtimeAdaptive
// only catches up when that frame has already crossed the stale threshold.
enum class VideoPresentationMode : uint8_t {
    BufferedFifo,
    RealtimeAdaptive,
};

inline const char* videoPresentationModeName(VideoPresentationMode mode) {
    switch (mode) {
        case VideoPresentationMode::BufferedFifo: return "buffered-fifo";
        case VideoPresentationMode::RealtimeAdaptive:
            return "realtime-adaptive";
    }
    return "unknown";
}

inline constexpr uint64_t kRealtimePresentationStaleUs = 25'000;

constexpr size_t stalePresentationFramesToDrop(VideoPresentationMode mode,
                                                size_t pending_frames,
                                                uint64_t oldest_wait_us) {
    return mode == VideoPresentationMode::RealtimeAdaptive &&
               pending_frames > 1 &&
               oldest_wait_us >= kRealtimePresentationStaleUs
        ? pending_frames - 1
        : 0;
}

// H.264 access units must still be decoded in order so reference frames stay
// valid. When the decode worker falls behind, however, publishing every stale
// decoded frame merely transfers the backlog into the renderer. Catch-up mode
// keeps decoding the dependency chain and suppresses presentation until the
// worker reaches the newest queued access unit.
enum class VideoDecodeCatchUpMode : uint8_t {
    Disabled,
    Realtime,
    Resilient,
};

struct VideoDecodeCatchUpDecision {
    bool active = false;
    bool suppress_output = false;
};

constexpr VideoDecodeCatchUpDecision videoDecodeCatchUpDecision(
    VideoDecodeCatchUpMode mode,
    bool already_active,
    size_t queued_behind,
    uint64_t packet_queue_age_us) {
    if (mode == VideoDecodeCatchUpMode::Disabled) {
        return {};
    }

    const size_t backlog_threshold =
        mode == VideoDecodeCatchUpMode::Realtime ? 2 : 4;
    const size_t stale_backlog_threshold =
        mode == VideoDecodeCatchUpMode::Realtime ? 1 : 2;
    const uint64_t stale_age_us =
        mode == VideoDecodeCatchUpMode::Realtime ? 20'000 : 60'000;
    bool active = already_active ||
        queued_behind >= backlog_threshold ||
        (queued_behind >= stale_backlog_threshold &&
         packet_queue_age_us >= stale_age_us);
    const bool suppress_output = active && queued_behind > 0;
    if (active && queued_behind == 0) active = false;
    return {active, suppress_output};
}

inline const char* videoDecodeCatchUpModeName(VideoDecodeCatchUpMode mode) {
    switch (mode) {
        case VideoDecodeCatchUpMode::Disabled: return "disabled";
        case VideoDecodeCatchUpMode::Realtime: return "realtime";
        case VideoDecodeCatchUpMode::Resilient: return "resilient";
    }
    return "unknown";
}

enum class AudioLatencyMode : uint8_t {
    Realtime,
    Balanced,
    Resilient,
};

struct AudioBufferConfig {
    size_t audren_frames_per_buffer = 5;
    size_t buffer_count = 5;
    size_t max_writer_wait_frames = 5;
};

constexpr AudioBufferConfig audioBufferConfig(AudioLatencyMode mode) {
    // One Audren frame is 5 ms at 48 kHz. Keep every mode aligned to Xbox's
    // common 20 ms Opus packet duration so latency changes only grow or shrink
    // the live descriptor ring; changing wave-buffer geometry mid-stream would
    // reinterpret offsets into the Audren memory pool. Normal playback uses a
    // 60 ms ring, while degraded paths can expand to 80 or 100 ms without
    // rebuilding the audio device. Every mode tolerates one 20 ms Opus period
    // of writer scheduling jitter before shedding stale burst audio. The old
    // 40 ms/two-buffer realtime ring underruns on routine 30--50 ms stalls.
    switch (mode) {
        case AudioLatencyMode::Realtime:
            return AudioBufferConfig{4, 3, 4};
        case AudioLatencyMode::Balanced:
            return AudioBufferConfig{4, 4, 4};
        case AudioLatencyMode::Resilient:
            return AudioBufferConfig{4, 5, 4};
    }
    return AudioBufferConfig{4, 5, 4};
}

constexpr size_t audioBufferCapacityMs(AudioLatencyMode mode) {
    const auto config = audioBufferConfig(mode);
    return config.audren_frames_per_buffer * 5 * config.buffer_count;
}

constexpr size_t audioIngressQueuePacketLimit(AudioLatencyMode mode) {
    // Opus is normally 20 ms per packet. These are emergency limits for a
    // stalled/startup receive burst, not steady-state prebuffer targets. Once
    // exceeded, the media worker abandons stale PCM and resumes at the live
    // edge so a one-second libpeer startup queue cannot become permanent
    // audible latency.
    switch (mode) {
        case AudioLatencyMode::Realtime: return 6;
        case AudioLatencyMode::Balanced: return 7;
        case AudioLatencyMode::Resilient: return 7;
    }
    return 7;
}

constexpr size_t audioStartupPrebufferPackets(AudioLatencyMode mode) {
    // Audio starts only after video is renderable. Keep a small cushion at
    // that boundary so low latency does not turn a routine packet bunch into
    // an immediate underrun. Recovery remains deeper by design.
    switch (mode) {
        case AudioLatencyMode::Realtime: return 3;
        case AudioLatencyMode::Balanced: return 3;
        case AudioLatencyMode::Resilient: return 4;
    }
    return 4;
}

inline const char* audioLatencyModeName(AudioLatencyMode mode) {
    switch (mode) {
        case AudioLatencyMode::Realtime: return "realtime";
        case AudioLatencyMode::Balanced: return "balanced";
        case AudioLatencyMode::Resilient: return "resilient";
    }
    return "unknown";
}

} // namespace lunar::stream
