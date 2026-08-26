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
};

constexpr AudioBufferConfig audioBufferConfig(AudioLatencyMode mode) {
    // One Audren frame is 5 ms at 48 kHz. The realtime geometry is aligned to
    // Xbox's common 20 ms Opus packet duration and caps hardware buffering at
    // 80 ms. The resilient geometry retains the established 125 ms capacity.
    switch (mode) {
        case AudioLatencyMode::Realtime:
            return AudioBufferConfig{4, 4};
        case AudioLatencyMode::Balanced:
            return AudioBufferConfig{5, 4};
        case AudioLatencyMode::Resilient:
            return AudioBufferConfig{5, 5};
    }
    return AudioBufferConfig{5, 5};
}

constexpr size_t audioBufferCapacityMs(AudioLatencyMode mode) {
    const auto config = audioBufferConfig(mode);
    return config.audren_frames_per_buffer * 5 * config.buffer_count;
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
