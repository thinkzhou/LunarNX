#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lunar::webrtc {

enum class NetworkPathMode : uint8_t {
    Home,
    Cloud,
};

enum class NetworkPathQuality : uint8_t {
    Good,
    Fair,
    Poor,
};

inline const char* networkPathQualityName(NetworkPathQuality quality) {
    switch (quality) {
        case NetworkPathQuality::Good: return "good";
        case NetworkPathQuality::Fair: return "fair";
        case NetworkPathQuality::Poor: return "poor";
    }
    return "unknown";
}

// Cumulative transport counters plus the current queue/RTT gauges. The
// estimator is sampled once per receiver-feedback window.
struct NetworkPathSample {
    uint64_t video_rtp_packets = 0;
    uint64_t video_payload_bytes = 0;
    uint64_t video_missing_detected = 0;
    uint64_t video_missing_recovered = 0;
    // Missing packets which were still absent when their frame deadline
    // expired. Unlike detected-recovered arithmetic, this remains correct
    // when a retransmission crosses a one-second sampling boundary.
    uint64_t video_missing_unrecovered = 0;
    uint64_t rtp_queue_drops = 0;
    uint32_t rtp_queue_depth = 0;
    uint32_t rtt_ms = 0;
    uint32_t interval_ms = 1000;
};

struct NetworkPathEstimate {
    bool valid = false;
    uint64_t sequence = 0;
    NetworkPathMode mode = NetworkPathMode::Home;
    NetworkPathQuality quality = NetworkPathQuality::Good;
    NetworkPathQuality observed_quality = NetworkPathQuality::Good;
    uint32_t window_packets = 0;
    uint32_t detected_missing = 0;
    uint32_t recovered_missing = 0;
    uint32_t unrecovered_missing = 0;
    uint64_t detected_loss_ppm = 0;
    uint64_t unrecovered_loss_ppm = 0;
    uint32_t queue_drops = 0;
    uint32_t queue_depth = 0;
    uint32_t raw_rtt_ms = 0;
    uint32_t smoothed_rtt_ms = 0;
    uint32_t baseline_rtt_ms = 0;
    uint32_t rtt_inflation_ms = 0;
    uint32_t received_bitrate_kbps = 0;
};

class NetworkPathEstimator {
public:
    explicit NetworkPathEstimator(NetworkPathMode mode) {
        reset(mode);
    }

    void reset(NetworkPathMode mode) {
        mode_ = mode;
        previous_ = {};
        estimate_ = {};
        estimate_.mode = mode;
        have_baseline_ = false;
        smoothed_rtt_ms_ = 0;
        rtt_history_.fill(0);
        rtt_history_size_ = 0;
        rtt_history_next_ = 0;
        quality_ = NetworkPathQuality::Good;
        bad_windows_ = 0;
        good_windows_ = 0;
        sequence_ = 0;
    }

    NetworkPathEstimate observe(const NetworkPathSample& sample) {
        updateRtt(sample.rtt_ms);
        if (!have_baseline_) {
            previous_ = sample;
            have_baseline_ = true;
            refreshRttFields(sample.rtt_ms);
            return estimate_;
        }

        NetworkPathEstimate next;
        next.valid = true;
        next.sequence = ++sequence_;
        next.mode = mode_;
        next.window_packets = narrow(delta(sample.video_rtp_packets,
                                           previous_.video_rtp_packets));
        next.detected_missing = narrow(delta(
            sample.video_missing_detected,
            previous_.video_missing_detected));
        next.recovered_missing = narrow(delta(
            sample.video_missing_recovered,
            previous_.video_missing_recovered));
        next.unrecovered_missing = narrow(delta(
            sample.video_missing_unrecovered,
            previous_.video_missing_unrecovered));
        next.queue_drops = narrow(delta(sample.rtp_queue_drops,
                                        previous_.rtp_queue_drops));
        next.queue_depth = sample.rtp_queue_depth;
        next.raw_rtt_ms = sample.rtt_ms;
        next.smoothed_rtt_ms = smoothed_rtt_ms_;
        next.baseline_rtt_ms = baselineRttMs();
        next.rtt_inflation_ms = sample.rtt_ms > next.baseline_rtt_ms
            ? sample.rtt_ms - next.baseline_rtt_ms : 0;

        const uint64_t total = static_cast<uint64_t>(next.window_packets) +
            next.detected_missing;
        next.detected_loss_ppm = total == 0 ? 0 :
            static_cast<uint64_t>(next.detected_missing) * 1'000'000u / total;
        next.unrecovered_loss_ppm = total == 0 ? 0 :
            static_cast<uint64_t>(next.unrecovered_missing) * 1'000'000u / total;

        const uint64_t payload_bytes = delta(sample.video_payload_bytes,
                                             previous_.video_payload_bytes);
        const uint32_t interval_ms = std::max<uint32_t>(1, sample.interval_ms);
        const uint64_t bitrate_kbps = payload_bytes >
                std::numeric_limits<uint64_t>::max() / 8u
            ? std::numeric_limits<uint64_t>::max()
            : payload_bytes * 8u / interval_ms;
        next.received_bitrate_kbps = narrow(bitrate_kbps);

        next.observed_quality = classify(next);
        updateQuality(next.observed_quality);
        next.quality = quality_;
        previous_ = sample;
        estimate_ = next;
        return estimate_;
    }

    const NetworkPathEstimate& estimate() const { return estimate_; }

private:
    static constexpr size_t kRttBaselineWindow = 30;

    static uint64_t delta(uint64_t current, uint64_t previous) {
        return current >= previous ? current - previous : current;
    }

    static uint32_t narrow(uint64_t value) {
        return static_cast<uint32_t>(std::min<uint64_t>(
            value, std::numeric_limits<uint32_t>::max()));
    }

    void updateRtt(uint32_t rtt_ms) {
        if (rtt_ms == 0) return;
        if (smoothed_rtt_ms_ == 0) {
            smoothed_rtt_ms_ = rtt_ms;
        } else {
            smoothed_rtt_ms_ = static_cast<uint32_t>(
                (static_cast<uint64_t>(smoothed_rtt_ms_) * 7u + rtt_ms) / 8u);
        }
        rtt_history_[rtt_history_next_] = rtt_ms;
        rtt_history_next_ = (rtt_history_next_ + 1) % kRttBaselineWindow;
        rtt_history_size_ = std::min(kRttBaselineWindow,
                                     rtt_history_size_ + 1);
    }

    uint32_t baselineRttMs() const {
        uint32_t baseline = 0;
        for (size_t index = 0; index < rtt_history_size_; ++index) {
            const uint32_t sample = rtt_history_[index];
            if (sample > 0 && (baseline == 0 || sample < baseline)) {
                baseline = sample;
            }
        }
        return baseline;
    }

    void refreshRttFields(uint32_t raw_rtt_ms) {
        estimate_.raw_rtt_ms = raw_rtt_ms;
        estimate_.smoothed_rtt_ms = smoothed_rtt_ms_;
        estimate_.baseline_rtt_ms = baselineRttMs();
        estimate_.rtt_inflation_ms = raw_rtt_ms > estimate_.baseline_rtt_ms
            ? raw_rtt_ms - estimate_.baseline_rtt_ms : 0;
    }

    NetworkPathQuality classify(const NetworkPathEstimate& path) const {
        const uint32_t good_inflation = mode_ == NetworkPathMode::Home
            ? 15u : 30u;
        const uint32_t fair_inflation = mode_ == NetworkPathMode::Home
            ? 35u : 80u;
        const bool good = path.queue_drops == 0 &&
            path.queue_depth < 512 &&
            path.unrecovered_loss_ppm < 2'000 &&
            path.detected_loss_ppm < 5'000 &&
            path.rtt_inflation_ms <= good_inflation;
        if (good) return NetworkPathQuality::Good;

        const bool fair = path.queue_drops == 0 &&
            path.queue_depth < 1024 &&
            path.unrecovered_loss_ppm < 10'000 &&
            path.rtt_inflation_ms <= fair_inflation;
        return fair ? NetworkPathQuality::Fair : NetworkPathQuality::Poor;
    }

    void updateQuality(NetworkPathQuality observed) {
        const auto rank = [](NetworkPathQuality quality) {
            return static_cast<int>(quality);
        };
        if (rank(observed) > rank(quality_)) {
            ++bad_windows_;
            good_windows_ = 0;
            if (bad_windows_ >= 2) {
                quality_ = observed;
                bad_windows_ = 0;
            }
        } else if (rank(observed) < rank(quality_)) {
            ++good_windows_;
            bad_windows_ = 0;
            if (good_windows_ >= 3) {
                quality_ = observed;
                good_windows_ = 0;
            }
        } else {
            bad_windows_ = 0;
            good_windows_ = 0;
        }
    }

    NetworkPathMode mode_ = NetworkPathMode::Home;
    NetworkPathSample previous_{};
    NetworkPathEstimate estimate_{};
    bool have_baseline_ = false;
    uint32_t smoothed_rtt_ms_ = 0;
    std::array<uint32_t, kRttBaselineWindow> rtt_history_{};
    size_t rtt_history_size_ = 0;
    size_t rtt_history_next_ = 0;
    NetworkPathQuality quality_ = NetworkPathQuality::Good;
    uint32_t bad_windows_ = 0;
    uint32_t good_windows_ = 0;
    uint64_t sequence_ = 0;
};

} // namespace lunar::webrtc
