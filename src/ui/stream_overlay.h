#pragma once
#ifdef __SWITCH__
#include <borealis.hpp>
#include "../stream/perf_stats.h"
#include <chrono>
#include <cstdint>
#include <string>

namespace lunar::ui {

/// Compact XStreaming-style quality bar shown during streaming.
class StreamOverlay : public brls::Box {
public:
    explicit StreamOverlay(const stream::PerfStats* perf);
    void update(float fps, const std::string& resolution);

private:
    const stream::PerfStats* perf_ = nullptr;
    brls::Label* metrics_label_ = nullptr;
    uint64_t last_encoded_bytes_ = 0;
    uint64_t last_decode_total_us_ = 0;
    uint32_t last_decode_samples_ = 0;
    std::chrono::steady_clock::time_point last_sample_time_{};
    float bitrate_mbps_ = 0.0f;
    float decode_ms_ = 0.0f;
};

} // namespace lunar::ui
#endif
