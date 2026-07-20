#pragma once

#include <cstddef>
#include <cstdint>

namespace lunar::stream {

inline uint64_t audioSamplesToNanoseconds(size_t samples, int sample_rate) {
    if (sample_rate <= 0) return 0;
    return (static_cast<uint64_t>(samples) * 1'000'000'000ULL) /
           static_cast<uint64_t>(sample_rate);
}

inline uint64_t estimateAudioPlaybackTimestamp(uint64_t frame_start_ns,
                                               size_t frame_samples,
                                               int sample_rate,
                                               size_t queued_samples) {
    const uint64_t frame_end_ns =
        frame_start_ns + audioSamplesToNanoseconds(frame_samples, sample_rate);
    const uint64_t queued_ns = audioSamplesToNanoseconds(queued_samples, sample_rate);
    return frame_end_ns > queued_ns ? frame_end_ns - queued_ns : 0;
}

} // namespace lunar::stream
