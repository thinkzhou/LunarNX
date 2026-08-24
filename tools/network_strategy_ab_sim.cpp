// Deterministic differential harness for the pre-change and current Xbox
// receiver strategies. It models one-second feedback windows, a capacity
// ceiling, first-transmission loss, NACK return delay, and long-tail recovery.
// It compares policies on identical seeded traces; it does not claim absolute
// real-hardware latency.
#include "app/adaptive_bitrate_controller.h"
#include "webrtc/network_path_estimator.h"
#include "webrtc/video_jitter_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace {

using lunar::app::AdaptiveBitrateController;
using lunar::webrtc::NetworkPathEstimate;
using lunar::webrtc::NetworkPathEstimator;
using lunar::webrtc::NetworkPathMode;
using lunar::webrtc::NetworkPathQuality;
using lunar::webrtc::NetworkPathSample;
using lunar::webrtc::VideoJitterPolicy;
using lunar::webrtc::computeVideoJitterPolicy;

constexpr uint32_t kFramesPerSecond = 60;
constexpr uint64_t kFrameIntervalUs = 16667;

struct Phase {
    int seconds = 1;
    uint32_t capacity_kbps = 20000;
    uint32_t base_rtt_ms = 75;
    uint32_t rtt_jitter_ms = 0;
    uint32_t random_loss_ppm = 0;
    uint32_t recoverable_ppm = 980000;
    uint32_t retransmit_jitter_ms = 20;
    uint32_t tail_probability_ppm = 0;
    uint32_t tail_delay_ms = 0;
};

struct Scenario {
    std::string name;
    NetworkPathMode mode = NetworkPathMode::Home;
    int maximum_kbps = 20000;
    std::vector<Phase> phases;
};

struct PathSecond {
    Phase phase;
    uint32_t raw_rtt_ms = 0;
};

struct WindowResult {
    uint32_t offered_packets = 0;
    uint32_t received_packets = 0;
    uint32_t detected_missing = 0;
    uint32_t recovered_missing = 0;
    uint32_t unrecovered_missing = 0;
    uint32_t queue_depth = 0;
    uint32_t queue_drops = 0;
    uint32_t delivered_kbps = 0;
    std::vector<uint32_t> gap_wait_ms;
    std::vector<uint32_t> final_gap_wait_ms;
    std::array<uint32_t, kFramesPerSecond> frame_wait_ms{};
    std::array<bool, kFramesPerSecond> frame_final{};
};

struct SimulationResult {
    std::string policy;
    double average_target_mbps = 0.0;
    double average_delivered_mbps = 0.0;
    double final_loss_percent = 0.0;
    double recovery_percent = 100.0;
    uint32_t gap_wait_p95_ms = 0;
    uint32_t final_gap_wait_p95_ms = 0;
    uint32_t maximum_gap_wait_ms = 0;
    uint32_t congested_seconds = 0;
    uint32_t poor_seconds = 0;
    uint32_t bitrate_transitions = 0;
    double network_frame_drop_percent = 0.0;
    double presentation_miss_percent = 0.0;
    uint32_t receiver_latency_p95_ms = 0;
    // For an aggregate result, *_max_ms is the mean of each trial's maximum;
    // *_worst_ms preserves the absolute maximum across all seeded trials.
    uint32_t receiver_latency_max_ms = 0;
    uint32_t receiver_latency_worst_ms = 0;
    uint32_t longest_freeze_ms = 0;
    uint32_t longest_freeze_worst_ms = 0;
    uint32_t frame_hold_max_ms = 0;
    uint32_t missing_hold_max_ms = 0;
    uint32_t recovery_hold_max_ms = 0;
    std::vector<int> target_kbps;
};

struct FrameCompletion {
    uint64_t nominal_us = 0;
    uint64_t completion_us = 0;
};

uint64_t mix(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

uint32_t deterministicRange(uint64_t seed, uint32_t limit) {
    return limit == 0 ? 0 : static_cast<uint32_t>(mix(seed) % limit);
}

uint32_t percentile95(std::vector<uint32_t> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::ceil(values.size() * 0.95)) - 1;
    return values[std::min(index, values.size() - 1)];
}

uint64_t effectiveMissingDeadlineMs(const VideoJitterPolicy& policy) {
    // Once enough later frames accumulate, VideoRtpJitterBuffer may release
    // the head frame at the head-blocked deadline before its ordinary missing
    // deadline. Model that effective bound rather than assuming the larger
    // configured hold is always available.
    const uint64_t backlog_formation_ms =
        (static_cast<uint64_t>(policy.max_head_blocked_frames) *
         kFrameIntervalUs + 999u) / 1000u;
    const uint64_t head_deadline_ms = std::max<uint64_t>(
        backlog_formation_ms, policy.head_blocked_hold_ms);
    return std::min<uint64_t>(policy.missing_packet_hold_ms,
                              head_deadline_ms);
}

PathSecond pathAt(const Scenario& scenario, int second) {
    int cursor = second;
    Phase selected = scenario.phases.back();
    for (const auto& phase : scenario.phases) {
        if (cursor < phase.seconds) {
            selected = phase;
            break;
        }
        cursor -= phase.seconds;
    }
    const int32_t jitter_span = static_cast<int32_t>(selected.rtt_jitter_ms);
    const uint32_t jitter_bucket = deterministicRange(
        mix(static_cast<uint64_t>(second) + scenario.name.size()),
        selected.rtt_jitter_ms * 2u + 1u);
    const int32_t signed_jitter = static_cast<int32_t>(jitter_bucket) -
        jitter_span;
    const int32_t raw_rtt = static_cast<int32_t>(selected.base_rtt_ms) +
        signed_jitter;
    return {selected, static_cast<uint32_t>(std::max<int32_t>(1, raw_rtt))};
}

WindowResult simulateWindow(int target_kbps,
                            uint64_t missing_deadline_ms,
                            const PathSecond& path,
                            uint64_t seed) {
    WindowResult result;
    result.offered_packets = std::max<uint32_t>(
        1, static_cast<uint32_t>(target_kbps / 8));
    const uint32_t capacity_packets = std::max<uint32_t>(
        1, path.phase.capacity_kbps / 8u);
    const uint32_t admitted_packets = std::min(
        result.offered_packets, capacity_packets);
    const uint32_t overload_missing = result.offered_packets -
        admitted_packets;
    const uint32_t random_missing = static_cast<uint32_t>(
        (static_cast<uint64_t>(admitted_packets) *
         path.phase.random_loss_ppm + 500000u) / 1000000u);
    result.detected_missing = overload_missing + random_missing;

    if (overload_missing > 0) {
        const uint64_t excess_ppm = static_cast<uint64_t>(overload_missing) *
            1000000u / result.offered_packets;
        result.queue_depth = static_cast<uint32_t>(std::min<uint64_t>(
            2048u, 256u + excess_ppm * 2048u / 1000000u));
    }

    result.gap_wait_ms.reserve(result.detected_missing);
    result.final_gap_wait_ms.reserve(result.detected_missing);
    for (uint32_t index = 0; index < overload_missing; ++index) {
        const auto wait_ms = static_cast<uint32_t>(missing_deadline_ms);
        const uint32_t frame = deterministicRange(
            seed ^ (static_cast<uint64_t>(index) * 0x9e3779b9ULL),
            kFramesPerSecond);
        result.gap_wait_ms.push_back(wait_ms);
        result.final_gap_wait_ms.push_back(wait_ms);
        result.frame_wait_ms[frame] = std::max(
            result.frame_wait_ms[frame], wait_ms);
        result.frame_final[frame] = true;
    }
    for (uint32_t index = 0; index < random_missing; ++index) {
        const uint64_t event_seed = mix(seed ^ (static_cast<uint64_t>(index) << 1));
        const bool can_recover = deterministicRange(event_seed, 1000000u) <
            path.phase.recoverable_ppm;
        uint32_t delay_ms = path.raw_rtt_ms + 4u + deterministicRange(
            event_seed ^ 0x51ed2705ULL,
            path.phase.retransmit_jitter_ms + 1u);
        if (deterministicRange(event_seed ^ 0x94d049bbULL, 1000000u) <
            path.phase.tail_probability_ppm) {
            delay_ms += path.phase.tail_delay_ms;
        }
        const uint32_t frame = deterministicRange(
            event_seed ^ 0xa0761d6478bd642fULL, kFramesPerSecond);
        if (can_recover && delay_ms <= missing_deadline_ms) {
            ++result.recovered_missing;
            result.gap_wait_ms.push_back(delay_ms);
            result.frame_wait_ms[frame] = std::max(
                result.frame_wait_ms[frame], delay_ms);
        } else {
            const auto wait_ms = static_cast<uint32_t>(missing_deadline_ms);
            result.gap_wait_ms.push_back(wait_ms);
            result.final_gap_wait_ms.push_back(wait_ms);
            result.frame_wait_ms[frame] = std::max(
                result.frame_wait_ms[frame], wait_ms);
            result.frame_final[frame] = true;
        }
    }
    result.unrecovered_missing = result.detected_missing -
        result.recovered_missing;
    result.received_packets = result.offered_packets -
        result.unrecovered_missing;
    result.delivered_kbps = static_cast<uint32_t>(
        static_cast<uint64_t>(target_kbps) * result.received_packets /
        result.offered_packets);
    return result;
}

enum class LegacyQuality : uint8_t { Good, Fair, Poor };

class LegacyPolicy {
public:
    LegacyPolicy(NetworkPathMode mode, int maximum_kbps)
        : mode_(mode), target_kbps_(maximum_kbps) {}

    int targetKbps() const { return target_kbps_; }

    VideoJitterPolicy jitterPolicy() const {
        VideoJitterPolicy policy;
        const uint64_t rtt_ms = smoothed_rtt_ms_;
        const bool cloud = mode_ == NetworkPathMode::Cloud;
        policy.frame_hold_ms = cloud
            ? std::clamp<uint64_t>(rtt_ms + 40u, 80u, 180u)
            : std::clamp<uint64_t>(24u + rtt_ms / 4u, 24u, 48u);
        policy.recovery_hold_ms = cloud
            ? std::clamp<uint64_t>(180u + rtt_ms * 2u, 300u, 500u)
            : std::min<uint64_t>(500u, 96u + rtt_ms / 2u);
        policy.max_head_blocked_frames = cloud ? 6u : 2u;
        policy.head_blocked_hold_ms = cloud
            ? std::clamp<uint64_t>(rtt_ms + 30u, 80u, 140u) : 32u;
        policy.missing_packet_hold_ms = legacyMissingHold(policy.frame_hold_ms,
                                                          rtt_ms);
        return policy;
    }

    void observe(const PathSecond& path, const WindowResult& window) {
        updateRtt(path.raw_rtt_ms);
        if (!quality_baseline_) {
            quality_baseline_ = true;
            return;
        }
        const uint64_t total = static_cast<uint64_t>(window.received_packets) +
            window.detected_missing;
        const uint64_t loss_ppm = total == 0 ? 0 :
            static_cast<uint64_t>(window.detected_missing) * 1000000u / total;
        const uint32_t arrival_gap_ms = window.detected_missing == 0 ? 1u :
            std::max<uint32_t>(1u, path.phase.rtt_jitter_ms);
        const uint32_t gap_packets = window.detected_missing == 0 ? 0u :
            std::min<uint32_t>(255u, std::max<uint32_t>(
                1u, window.detected_missing / 60u));
        const bool good = smoothed_rtt_ms_ > 0 && smoothed_rtt_ms_ <= 20u &&
            loss_ppm < 2000u && arrival_gap_ms <= 8u && gap_packets <= 2u;
        const bool fair = smoothed_rtt_ms_ > 0 && smoothed_rtt_ms_ <= 50u &&
            loss_ppm < 10000u && arrival_gap_ms <= 20u && gap_packets <= 5u;
        const LegacyQuality observed = good ? LegacyQuality::Good :
            fair ? LegacyQuality::Fair : LegacyQuality::Poor;
        updateQuality(observed);
    }

    bool poor() const { return quality_ == LegacyQuality::Poor; }

private:
    uint64_t legacyMissingHold(uint64_t frame_hold_ms,
                               uint64_t rtt_ms) const {
        if (rtt_ms == 0) return frame_hold_ms;
        const bool home_profile = frame_hold_ms <= 48u;
        if (quality_ == LegacyQuality::Good && rtt_ms < 150u) {
            const uint64_t cap = home_profile ? 32u : 120u;
            return std::max(frame_hold_ms, std::min(cap, rtt_ms + 8u));
        }
        if (quality_ == LegacyQuality::Fair) {
            const uint64_t cap = home_profile ? 60u : 180u;
            const uint64_t safety = home_profile ? 20u : 40u;
            return std::max(frame_hold_ms,
                            std::min(cap, rtt_ms + safety));
        }
        if (rtt_ms >= 150u || !home_profile) {
            return std::max(frame_hold_ms,
                            std::min<uint64_t>(500u,
                                rtt_ms + frame_hold_ms + 20u));
        }
        return std::max(frame_hold_ms,
                        std::min<uint64_t>(180u,
                            rtt_ms + frame_hold_ms + 20u));
    }

    void updateRtt(uint32_t sample_ms) {
        if (sample_ms == 0 || sample_ms == last_rtt_sample_ms_) return;
        last_rtt_sample_ms_ = sample_ms;
        if (smoothed_rtt_ms_ == 0) {
            smoothed_rtt_ms_ = sample_ms;
            return;
        }
        const uint64_t bounded = sample_ms > smoothed_rtt_ms_
            ? std::min<uint64_t>(sample_ms, smoothed_rtt_ms_ + 32u)
            : std::max<uint64_t>(sample_ms,
                smoothed_rtt_ms_ > 32u ? smoothed_rtt_ms_ - 32u : 1u);
        smoothed_rtt_ms_ = (smoothed_rtt_ms_ * 7u + bounded) / 8u;
    }

    void updateQuality(LegacyQuality observed) {
        const auto rank = [](LegacyQuality quality) {
            return static_cast<int>(quality);
        };
        if (rank(observed) > rank(quality_)) {
            ++bad_windows_;
            good_windows_ = 0;
            if (bad_windows_ >= 2u) {
                quality_ = observed;
                bad_windows_ = 0;
            }
        } else if (rank(observed) < rank(quality_)) {
            ++good_windows_;
            bad_windows_ = 0;
            if (good_windows_ >= 3u) {
                quality_ = observed;
                good_windows_ = 0;
            }
        } else {
            bad_windows_ = 0;
            good_windows_ = 0;
        }
    }

    NetworkPathMode mode_ = NetworkPathMode::Home;
    int target_kbps_ = 20000;
    uint64_t smoothed_rtt_ms_ = 0;
    uint64_t last_rtt_sample_ms_ = 0;
    LegacyQuality quality_ = LegacyQuality::Good;
    bool quality_baseline_ = false;
    uint32_t bad_windows_ = 0;
    uint32_t good_windows_ = 0;
};

class NewPolicy {
public:
    NewPolicy(NetworkPathMode mode, int maximum_kbps)
        : mode_(mode), estimator_(mode), controller_(mode, maximum_kbps) {
        jitter_policy_ = computeVideoJitterPolicy(mode_, estimator_.estimate());
    }

    int targetKbps() const { return controller_.targetKbps(); }
    const VideoJitterPolicy& jitterPolicy() const { return jitter_policy_; }

    void observe(const PathSecond& path, const WindowResult& window) {
        cumulative_.video_rtp_packets += window.received_packets;
        cumulative_.video_payload_bytes +=
            static_cast<uint64_t>(window.delivered_kbps) * 125u;
        cumulative_.video_missing_detected += window.detected_missing;
        cumulative_.video_missing_recovered += window.recovered_missing;
        cumulative_.video_missing_unrecovered += window.unrecovered_missing;
        cumulative_.rtp_queue_drops += window.queue_drops;
        cumulative_.rtp_queue_depth = window.queue_depth;
        cumulative_.rtt_ms = path.raw_rtt_ms;
        cumulative_.interval_ms = 1000;
        estimate_ = estimator_.observe(cumulative_);
        jitter_policy_ = computeVideoJitterPolicy(mode_, estimate_);
        controller_.observe(estimate_);
    }

    bool poor() const {
        return estimate_.quality == NetworkPathQuality::Poor;
    }

private:
    NetworkPathMode mode_ = NetworkPathMode::Home;
    NetworkPathEstimator estimator_;
    AdaptiveBitrateController controller_;
    NetworkPathSample cumulative_{};
    NetworkPathEstimate estimate_{};
    VideoJitterPolicy jitter_policy_{};
};

template <typename Policy>
SimulationResult runPolicy(const Scenario& scenario,
                           const std::string& policy_name,
                           uint64_t trial) {
    Policy policy(scenario.mode, scenario.maximum_kbps);
    SimulationResult result;
    result.policy = policy_name;
    uint64_t total_target_kbps = 0;
    uint64_t total_delivered_kbps = 0;
    uint64_t total_received = 0;
    uint64_t total_detected = 0;
    uint64_t total_recovered = 0;
    uint64_t total_unrecovered = 0;
    std::vector<uint32_t> gap_waits;
    std::vector<uint32_t> final_gap_waits;
    std::vector<FrameCompletion> frame_completions;
    uint64_t jitter_barrier_us = 0;
    uint64_t network_dropped_frames = 0;
    int previous_target = policy.targetKbps();
    int seconds = 0;
    for (const auto& phase : scenario.phases) seconds += phase.seconds;

    for (int second = 0; second < seconds; ++second) {
        const PathSecond path = pathAt(scenario, second);
        const int target_kbps = policy.targetKbps();
        const auto jitter = policy.jitterPolicy();
        result.frame_hold_max_ms = std::max<uint32_t>(
            result.frame_hold_max_ms,
            static_cast<uint32_t>(jitter.frame_hold_ms));
        result.missing_hold_max_ms = std::max<uint32_t>(
            result.missing_hold_max_ms,
            static_cast<uint32_t>(jitter.missing_packet_hold_ms));
        result.recovery_hold_max_ms = std::max<uint32_t>(
            result.recovery_hold_max_ms,
            static_cast<uint32_t>(jitter.recovery_hold_ms));
        const auto window = simulateWindow(
            target_kbps,
            effectiveMissingDeadlineMs(jitter),
            path,
            mix(static_cast<uint64_t>(second) ^
                (static_cast<uint64_t>(scenario.name.size()) << 32) ^
                (trial * 0xd6e8feb86659fd93ULL)));
        result.target_kbps.push_back(target_kbps);
        total_target_kbps += target_kbps;
        total_delivered_kbps += window.delivered_kbps;
        total_received += window.received_packets;
        total_detected += window.detected_missing;
        total_recovered += window.recovered_missing;
        total_unrecovered += window.unrecovered_missing;
        gap_waits.insert(gap_waits.end(), window.gap_wait_ms.begin(),
                         window.gap_wait_ms.end());
        final_gap_waits.insert(final_gap_waits.end(),
                               window.final_gap_wait_ms.begin(),
                               window.final_gap_wait_ms.end());
        for (uint32_t frame = 0; frame < kFramesPerSecond; ++frame) {
            const uint64_t frame_index =
                static_cast<uint64_t>(second) * kFramesPerSecond + frame;
            const uint64_t nominal_us = frame_index * kFrameIntervalUs;
            const uint64_t wait_us =
                static_cast<uint64_t>(window.frame_wait_ms[frame]) * 1000u;
            if (wait_us > 0) {
                jitter_barrier_us = std::max(jitter_barrier_us,
                                             nominal_us + wait_us);
            }
            if (window.frame_final[frame]) {
                ++network_dropped_frames;
                continue;
            }
            frame_completions.push_back({
                nominal_us,
                std::max(nominal_us + wait_us, jitter_barrier_us),
            });
        }
        if (static_cast<uint32_t>(target_kbps) > path.phase.capacity_kbps) {
            ++result.congested_seconds;
        }
        policy.observe(path, window);
        if (policy.poor()) ++result.poor_seconds;
        if (policy.targetKbps() != previous_target) {
            ++result.bitrate_transitions;
            previous_target = policy.targetKbps();
        }
    }

    result.average_target_mbps = static_cast<double>(total_target_kbps) /
        seconds / 1000.0;
    result.average_delivered_mbps = static_cast<double>(total_delivered_kbps) /
        seconds / 1000.0;
    const uint64_t total_packets = total_received + total_unrecovered;
    result.final_loss_percent = total_packets == 0 ? 0.0 :
        static_cast<double>(total_unrecovered) * 100.0 / total_packets;
    result.recovery_percent = total_detected == 0 ? 100.0 :
        static_cast<double>(total_recovered) * 100.0 / total_detected;
    result.gap_wait_p95_ms = percentile95(gap_waits);
    result.final_gap_wait_p95_ms = percentile95(final_gap_waits);
    result.maximum_gap_wait_ms = gap_waits.empty() ? 0 :
        *std::max_element(gap_waits.begin(), gap_waits.end());

    const uint64_t source_frames = static_cast<uint64_t>(seconds) *
        kFramesPerSecond;
    result.network_frame_drop_percent = source_frames == 0 ? 0.0 :
        static_cast<double>(network_dropped_frames) * 100.0 / source_frames;
    size_t completion_index = 0;
    uint64_t displayed_frames = 0;
    uint32_t consecutive_repeats = 0;
    uint32_t longest_repeats = 0;
    std::vector<uint32_t> displayed_latency_ms;
    for (uint64_t frame = 0; frame < source_frames; ++frame) {
        const uint64_t vsync_us = frame * kFrameIntervalUs;
        bool have_candidate = false;
        FrameCompletion candidate;
        while (completion_index < frame_completions.size() &&
               frame_completions[completion_index].completion_us <= vsync_us) {
            candidate = frame_completions[completion_index++];
            have_candidate = true;
        }
        if (have_candidate) {
            ++displayed_frames;
            consecutive_repeats = 0;
            displayed_latency_ms.push_back(static_cast<uint32_t>(
                (vsync_us - candidate.nominal_us) / 1000u));
        } else {
            ++consecutive_repeats;
            longest_repeats = std::max(longest_repeats,
                                       consecutive_repeats);
        }
    }
    result.presentation_miss_percent = source_frames == 0 ? 0.0 :
        static_cast<double>(source_frames - displayed_frames) * 100.0 /
        source_frames;
    result.receiver_latency_p95_ms = percentile95(displayed_latency_ms);
    result.receiver_latency_max_ms = displayed_latency_ms.empty() ? 0 :
        *std::max_element(displayed_latency_ms.begin(),
                          displayed_latency_ms.end());
    result.longest_freeze_ms = static_cast<uint32_t>(
        static_cast<uint64_t>(longest_repeats) * kFrameIntervalUs / 1000u);
    result.receiver_latency_worst_ms = result.receiver_latency_max_ms;
    result.longest_freeze_worst_ms = result.longest_freeze_ms;
    return result;
}

template <typename Policy>
SimulationResult runTrials(const Scenario& scenario,
                           const std::string& policy_name,
                           uint32_t trials) {
    SimulationResult aggregate;
    aggregate.policy = policy_name;
    double target = 0.0;
    double delivered = 0.0;
    double final_loss = 0.0;
    double recovery = 0.0;
    uint64_t gap_p95 = 0;
    uint64_t final_gap_p95 = 0;
    uint64_t maximum_gap = 0;
    uint64_t congested = 0;
    uint64_t poor = 0;
    uint64_t transitions = 0;
    double network_frame_drop = 0.0;
    double presentation_miss = 0.0;
    uint64_t receiver_latency_p95 = 0;
    uint64_t receiver_latency_max = 0;
    uint64_t longest_freeze = 0;
    uint64_t frame_hold_max = 0;
    uint64_t missing_hold_max = 0;
    uint64_t recovery_hold_max = 0;
    for (uint32_t trial = 0; trial < trials; ++trial) {
        const auto result = runPolicy<Policy>(scenario, policy_name, trial);
        if (trial == 0) aggregate.target_kbps = result.target_kbps;
        target += result.average_target_mbps;
        delivered += result.average_delivered_mbps;
        final_loss += result.final_loss_percent;
        recovery += result.recovery_percent;
        gap_p95 += result.gap_wait_p95_ms;
        final_gap_p95 += result.final_gap_wait_p95_ms;
        maximum_gap += result.maximum_gap_wait_ms;
        congested += result.congested_seconds;
        poor += result.poor_seconds;
        transitions += result.bitrate_transitions;
        network_frame_drop += result.network_frame_drop_percent;
        presentation_miss += result.presentation_miss_percent;
        receiver_latency_p95 += result.receiver_latency_p95_ms;
        receiver_latency_max += result.receiver_latency_max_ms;
        longest_freeze += result.longest_freeze_ms;
        aggregate.receiver_latency_worst_ms = std::max(
            aggregate.receiver_latency_worst_ms,
            result.receiver_latency_worst_ms);
        aggregate.longest_freeze_worst_ms = std::max(
            aggregate.longest_freeze_worst_ms,
            result.longest_freeze_worst_ms);
        frame_hold_max += result.frame_hold_max_ms;
        missing_hold_max += result.missing_hold_max_ms;
        recovery_hold_max += result.recovery_hold_max_ms;
    }
    aggregate.average_target_mbps = target / trials;
    aggregate.average_delivered_mbps = delivered / trials;
    aggregate.final_loss_percent = final_loss / trials;
    aggregate.recovery_percent = recovery / trials;
    aggregate.gap_wait_p95_ms = static_cast<uint32_t>(gap_p95 / trials);
    aggregate.final_gap_wait_p95_ms = static_cast<uint32_t>(
        final_gap_p95 / trials);
    aggregate.maximum_gap_wait_ms = static_cast<uint32_t>(maximum_gap / trials);
    aggregate.congested_seconds = static_cast<uint32_t>(congested / trials);
    aggregate.poor_seconds = static_cast<uint32_t>(poor / trials);
    aggregate.bitrate_transitions = static_cast<uint32_t>(transitions / trials);
    aggregate.network_frame_drop_percent = network_frame_drop / trials;
    aggregate.presentation_miss_percent = presentation_miss / trials;
    aggregate.receiver_latency_p95_ms = static_cast<uint32_t>(
        receiver_latency_p95 / trials);
    aggregate.receiver_latency_max_ms = static_cast<uint32_t>(
        receiver_latency_max / trials);
    aggregate.longest_freeze_ms = static_cast<uint32_t>(
        longest_freeze / trials);
    aggregate.frame_hold_max_ms = static_cast<uint32_t>(
        frame_hold_max / trials);
    aggregate.missing_hold_max_ms = static_cast<uint32_t>(
        missing_hold_max / trials);
    aggregate.recovery_hold_max_ms = static_cast<uint32_t>(
        recovery_hold_max / trials);
    return aggregate;
}

std::string targetTrace(const SimulationResult& result) {
    if (result.target_kbps.empty()) return "-";
    std::string trace;
    int start = 0;
    int value = result.target_kbps.front();
    for (size_t index = 1; index <= result.target_kbps.size(); ++index) {
        if (index < result.target_kbps.size() &&
            result.target_kbps[index] == value) {
            continue;
        }
        if (!trace.empty()) trace += " ";
        trace += std::to_string(start) + "-" + std::to_string(index - 1) +
            "s:" + std::to_string(value / 1000) + "M";
        if (index < result.target_kbps.size()) {
            start = static_cast<int>(index);
            value = result.target_kbps[index];
        }
    }
    return trace;
}

std::vector<Scenario> scenarios() {
    return {
        {"home_clean", NetworkPathMode::Home, 20000,
         {{30, 40000, 5, 2, 0, 980000, 8, 0, 0}}},
        {"home_clean_rtt_20", NetworkPathMode::Home, 20000,
         {{30, 40000, 20, 2, 0, 990000, 6, 0, 0}}},
        {"home_clean_rtt_50", NetworkPathMode::Home, 20000,
         {{30, 40000, 50, 3, 0, 990000, 8, 0, 0}}},
        {"home_rtt_5_single_loss", NetworkPathMode::Home, 20000,
         {{10, 40000, 5, 1, 0, 990000, 4, 0, 0},
          {1, 40000, 5, 1, 20000, 990000, 4, 0, 0},
          {19, 40000, 5, 1, 0, 990000, 4, 0, 0}}},
        {"home_rtt_20_single_loss", NetworkPathMode::Home, 20000,
         {{10, 40000, 20, 2, 0, 990000, 6, 0, 0},
          {1, 40000, 20, 2, 20000, 990000, 6, 0, 0},
          {19, 40000, 20, 2, 0, 990000, 6, 0, 0}}},
        {"home_rtt_35_single_loss", NetworkPathMode::Home, 20000,
         {{10, 40000, 35, 3, 0, 990000, 8, 0, 0},
          {1, 40000, 35, 3, 20000, 990000, 8, 0, 0},
          {19, 40000, 35, 3, 0, 990000, 8, 0, 0}}},
        {"home_rtt_50_single_loss", NetworkPathMode::Home, 20000,
         {{10, 40000, 50, 3, 0, 990000, 8, 0, 0},
          {1, 40000, 50, 3, 20000, 990000, 8, 0, 0},
          {19, 40000, 50, 3, 0, 990000, 8, 0, 0}}},
        {"home_rtt_80_single_loss", NetworkPathMode::Home, 20000,
         {{10, 40000, 80, 4, 0, 990000, 10, 0, 0},
          {1, 40000, 80, 4, 20000, 990000, 10, 0, 0},
          {19, 40000, 80, 4, 0, 990000, 10, 0, 0}}},
        {"home_one_second_rtt_spike", NetworkPathMode::Home, 20000,
         {{10, 40000, 5, 1, 0, 990000, 4, 0, 0},
          {1, 40000, 80, 5, 0, 990000, 10, 0, 0},
          {19, 40000, 5, 1, 0, 990000, 4, 0, 0}}},
        {"home_three_second_rtt_spike", NetworkPathMode::Home, 20000,
         {{10, 40000, 5, 1, 0, 990000, 4, 0, 0},
          {3, 40000, 80, 5, 0, 990000, 10, 0, 0},
          {17, 40000, 5, 1, 0, 990000, 4, 0, 0}}},
        {"home_rtt_spike_with_loss", NetworkPathMode::Home, 20000,
         {{10, 40000, 5, 1, 0, 990000, 4, 0, 0},
          {2, 40000, 60, 5, 20000, 990000, 12, 0, 0},
          {18, 40000, 5, 1, 0, 990000, 4, 0, 0}}},
        {"home_wifi_blips", NetworkPathMode::Home, 20000,
         {{10, 30000, 6, 3, 1000, 990000, 12, 0, 0},
          {8, 30000, 8, 12, 12000, 990000, 20, 10000, 45},
          {12, 30000, 6, 3, 1000, 990000, 12, 0, 0}}},
        {"home_capacity_30_to_12_to_30", NetworkPathMode::Home, 20000,
         {{8, 30000, 5, 2, 0, 980000, 8, 0, 0},
          {12, 12000, 5, 4, 2000, 970000, 18, 5000, 40},
          {20, 30000, 5, 2, 0, 980000, 8, 0, 0}}},
        {"home_rtt_step_without_loss", NetworkPathMode::Home, 20000,
         {{10, 40000, 5, 2, 0, 990000, 8, 0, 0},
          {12, 40000, 55, 3, 0, 990000, 8, 0, 0},
          {18, 40000, 5, 2, 0, 990000, 8, 0, 0}}},
        {"cloud_clean_hq", NetworkPathMode::Cloud, 30000,
         {{30, 40000, 75, 5, 0, 980000, 20, 0, 0}}},
        {"cloud_capacity_30_to_14_to_30", NetworkPathMode::Cloud, 30000,
         {{10, 30000, 75, 8, 1000, 990000, 20, 0, 0},
          {15, 14000, 90, 15, 3000, 970000, 35, 10000, 80},
          {40, 30000, 75, 8, 1000, 990000, 20, 0, 0}}},
        {"cloud_one_second_capacity_outage", NetworkPathMode::Cloud, 30000,
         {{10, 40000, 75, 5, 0, 990000, 20, 0, 0},
          {1, 5000, 100, 10, 5000, 950000, 40, 10000, 80},
          {29, 40000, 75, 5, 0, 990000, 20, 0, 0}}},
        {"cloud_periodic_bandwidth", NetworkPathMode::Cloud, 30000,
         {{5, 30000, 75, 8, 1000, 990000, 20, 0, 0},
          {5, 12000, 90, 12, 2000, 970000, 35, 10000, 70},
          {5, 30000, 75, 8, 1000, 990000, 20, 0, 0},
          {5, 12000, 90, 12, 2000, 970000, 35, 10000, 70},
          {5, 30000, 75, 8, 1000, 990000, 20, 0, 0},
          {5, 12000, 90, 12, 2000, 970000, 35, 10000, 70},
          {10, 30000, 75, 8, 1000, 990000, 20, 0, 0}}},
        {"cloud_bursty_recoverable_loss", NetworkPathMode::Cloud, 20000,
         {{10, 30000, 80, 8, 1000, 990000, 20, 0, 0},
          {12, 30000, 85, 15, 30000, 995000, 40, 20000, 70},
          {18, 30000, 80, 8, 1000, 990000, 20, 0, 0}}},
        {"cloud_persistent_random_loss", NetworkPathMode::Cloud, 20000,
         {{40, 30000, 80, 8, 60000, 0, 20, 0, 0}}},
        {"cloud_high_rtt_long_tail", NetworkPathMode::Cloud, 20000,
         {{40, 30000, 180, 15, 10000, 995000, 55, 50000, 100}}},
        {"cloud_very_high_rtt", NetworkPathMode::Cloud, 20000,
         {{40, 30000, 240, 20, 5000, 995000, 70, 50000, 120}}},
    };
}

void printResult(const Scenario& scenario,
                 const SimulationResult& old_result,
                 const SimulationResult& new_result) {
    const auto row = [&](const SimulationResult& result) {
        std::cout << std::left << std::setw(8) << result.policy
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(9) << result.average_target_mbps
                  << std::setw(10) << result.average_delivered_mbps
                  << std::setw(10) << result.final_loss_percent
                  << std::setw(10) << result.recovery_percent
                  << std::setw(10) << result.gap_wait_p95_ms
                  << std::setw(10) << result.final_gap_wait_p95_ms
                  << std::setw(9) << result.congested_seconds
                  << std::setw(8) << result.poor_seconds
                  << std::setw(8) << result.bitrate_transitions << '\n';
    };
    std::cout << "\n[" << scenario.name << "]\n"
              << "policy  targetM  goodputM finalLoss% recovery% gapP95ms"
                 " finalP95 overload poor transitions\n";
    row(old_result);
    row(new_result);
    std::cout << "        frameDrop% visibleMiss% latP95avg latMaxAvg latWorst"
                 " freezeMaxAvg freezeWorst holds(frame/missing/recovery)\n";
    const auto ux_row = [](const SimulationResult& result) {
        std::cout << std::left << std::setw(8) << result.policy
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(11) << result.network_frame_drop_percent
                  << std::setw(13) << result.presentation_miss_percent
                  << std::setw(13) << result.receiver_latency_p95_ms
                  << std::setw(13) << result.receiver_latency_max_ms
                  << std::setw(10) << result.receiver_latency_worst_ms
                  << std::setw(12) << result.longest_freeze_ms
                  << std::setw(12) << result.longest_freeze_worst_ms
                  << "   " << result.frame_hold_max_ms << "/"
                  << result.missing_hold_max_ms << "/"
                  << result.recovery_hold_max_ms << '\n';
    };
    ux_row(old_result);
    ux_row(new_result);
    std::cout << "old trace: " << targetTrace(old_result) << '\n'
              << "new trace: " << targetTrace(new_result) << '\n';
}

} // namespace

int main() {
    constexpr uint32_t kTrials = 100;
    int failures = 0;
    const auto check = [&failures](bool condition, const std::string& message) {
        if (condition) return;
        ++failures;
        std::cerr << "SIMULATION CHECK FAILED: " << message << '\n';
    };
    std::cout << "Deterministic Xbox network strategy A/B simulation ("
              << kTrials << " seeded trials)\n"
              << "Old = pre-change fixed REMB + absolute-RTT jitter policy\n"
              << "New = current estimator + adaptive REMB + mode-aware jitter\n"
              << "gapP95 = all detected gaps; finalP95 = deadline paid by gaps"
                 " that never recovered\n"
              << "*Avg = mean across per-trial values; *Worst = absolute max"
                 " across all trials\n";
    for (const auto& scenario : scenarios()) {
        const auto old_result = runTrials<LegacyPolicy>(
            scenario, "old", kTrials);
        const auto new_result = runTrials<NewPolicy>(
            scenario, "new", kTrials);
        printResult(scenario, old_result, new_result);

        const int final_target = new_result.target_kbps.empty() ? 0 :
            new_result.target_kbps.back();
        if (scenario.mode == NetworkPathMode::Cloud) {
            check(new_result.receiver_latency_worst_ms <= 200,
                  scenario.name + " must stay inside the modeled receiver latency budget");
        }
        if (scenario.name == "home_clean") {
            check(new_result.final_loss_percent == 0.0 &&
                      new_result.average_target_mbps == 20.0,
                  "clean home path must be regression-neutral");
            check(new_result.presentation_miss_percent == 0.0 &&
                      new_result.receiver_latency_p95_ms == 0,
                  "clean home path must add no modeled frame miss or latency");
        } else if (scenario.name == "home_clean_rtt_20" ||
                   scenario.name == "home_clean_rtt_50") {
            check(new_result.final_loss_percent == 0.0 &&
                      new_result.presentation_miss_percent == 0.0 &&
                      new_result.receiver_latency_max_ms == 0 &&
                      final_target == scenario.maximum_kbps,
                  scenario.name + " must add no delay on complete frames");
        } else if (scenario.name == "home_rtt_5_single_loss" ||
                   scenario.name == "home_rtt_20_single_loss" ||
                   scenario.name == "home_rtt_35_single_loss" ||
                   scenario.name == "home_rtt_50_single_loss" ||
                   scenario.name == "home_rtt_80_single_loss") {
            check(new_result.recovery_percent >= 95.0,
                  scenario.name + " must recover at least 95% of an isolated loss burst");
            check(new_result.network_frame_drop_percent <=
                      old_result.network_frame_drop_percent + 0.05 &&
                      new_result.longest_freeze_worst_ms <=
                          old_result.longest_freeze_worst_ms + 5,
                  scenario.name + " must preserve local frame continuity");
        } else if (scenario.name == "home_one_second_rtt_spike") {
            check(new_result.presentation_miss_percent == 0.0 &&
                      new_result.receiver_latency_max_ms == 0 &&
                      new_result.bitrate_transitions == 0,
                  "one RTT spike without loss must be invisible to the user");
        } else if (scenario.name == "home_three_second_rtt_spike") {
            check(new_result.presentation_miss_percent == 0.0 &&
                      new_result.receiver_latency_max_ms == 0 &&
                      final_target == scenario.maximum_kbps,
                  "short RTT step without loss must not leave lasting degradation");
        } else if (scenario.name == "home_rtt_spike_with_loss") {
            check(new_result.final_loss_percent <=
                      old_result.final_loss_percent &&
                      new_result.longest_freeze_worst_ms <=
                          old_result.longest_freeze_worst_ms,
                  "RTT and loss spike must improve final loss without a longer freeze");
        } else if (scenario.name == "home_wifi_blips") {
            check(new_result.network_frame_drop_percent <=
                      old_result.network_frame_drop_percent + 0.10 &&
                      new_result.presentation_miss_percent <=
                          old_result.presentation_miss_percent + 0.10 &&
                      new_result.longest_freeze_worst_ms <=
                          old_result.longest_freeze_worst_ms + 10,
                  "home Wi-Fi blips must stay within the low-latency trade-off bound");
        } else if (scenario.name == "home_capacity_30_to_12_to_30") {
            check(new_result.final_loss_percent <=
                      old_result.final_loss_percent * 0.25,
                  "home capacity dip must cut final loss by at least 75%");
            check(new_result.network_frame_drop_percent <=
                      old_result.network_frame_drop_percent * 0.25 &&
                      new_result.longest_freeze_worst_ms <=
                          old_result.longest_freeze_worst_ms * 0.25,
                  "home capacity dip must materially reduce frame loss and freeze");
        } else if (scenario.name == "home_rtt_step_without_loss") {
            const int minimum_target = *std::min_element(
                new_result.target_kbps.begin(), new_result.target_kbps.end());
            check(final_target == scenario.maximum_kbps &&
                      minimum_target >= 15000,
                  "RTT-only route shift must roll back without reaching floor");
        } else if (scenario.name == "cloud_clean_hq") {
            check(new_result.final_loss_percent == 0.0 &&
                      final_target == scenario.maximum_kbps,
                  "clean cloud HQ must probe to the configured cap");
            check(new_result.presentation_miss_percent == 0.0 &&
                      new_result.receiver_latency_p95_ms == 0,
                  "clean cloud HQ must add no modeled frame miss or latency");
        } else if (scenario.name == "cloud_capacity_30_to_14_to_30") {
            check(new_result.final_loss_percent <=
                      old_result.final_loss_percent * 0.25,
                  "cloud capacity dip must cut final loss by at least 75%");
            check(final_target == scenario.maximum_kbps,
                  "cloud capacity recovery must eventually restore the cap");
            check(new_result.network_frame_drop_percent <=
                      old_result.network_frame_drop_percent * 0.25 &&
                      new_result.presentation_miss_percent <=
                          old_result.presentation_miss_percent * 0.75 &&
                      new_result.longest_freeze_worst_ms <=
                          old_result.longest_freeze_worst_ms * 0.25,
                  "cloud capacity dip must reduce visible frame loss and freeze");
        } else if (scenario.name == "cloud_one_second_capacity_outage") {
            check(final_target == scenario.maximum_kbps,
                  "one-second outage must quickly restore the previous cap");
            check(new_result.final_loss_percent <=
                      old_result.final_loss_percent + 0.15 &&
                      new_result.network_frame_drop_percent <=
                          old_result.network_frame_drop_percent + 0.05 &&
                      new_result.presentation_miss_percent <=
                          old_result.presentation_miss_percent + 0.10 &&
                      new_result.longest_freeze_worst_ms <=
                          old_result.longest_freeze_worst_ms + 50,
                  "one-second outage must keep loss and freeze within the transient bound");
        } else if (scenario.name == "cloud_periodic_bandwidth") {
            check(new_result.final_loss_percent <=
                      old_result.final_loss_percent * 0.35,
                  "periodic bandwidth must cut final loss by at least 65%");
            check(new_result.network_frame_drop_percent <=
                      old_result.network_frame_drop_percent * 0.30 &&
                      new_result.presentation_miss_percent <=
                          old_result.presentation_miss_percent * 0.50,
                  "periodic bandwidth must materially reduce visible frame loss");
        } else if (scenario.name == "cloud_bursty_recoverable_loss") {
            check(new_result.recovery_percent >= 95.0,
                  "ordinary cloud burst must retain at least 95% recovery");
            check(new_result.presentation_miss_percent <=
                      old_result.presentation_miss_percent &&
                      new_result.longest_freeze_worst_ms <=
                          old_result.longest_freeze_worst_ms * 0.90 &&
                      new_result.network_frame_drop_percent <=
                          old_result.network_frame_drop_percent * 0.10,
                  "ordinary cloud burst must trade bounded latency for fewer broken frames and freezes");
        } else if (scenario.name == "cloud_persistent_random_loss") {
            check(new_result.final_loss_percent <=
                      old_result.final_loss_percent + 0.10,
                  "persistent random loss must not increase packet loss rate");
            check(new_result.network_frame_drop_percent <
                      old_result.network_frame_drop_percent &&
                      new_result.longest_freeze_worst_ms <
                          old_result.longest_freeze_worst_ms,
                  "persistent random loss must trade bitrate for fewer broken frames");
        } else if (scenario.name == "cloud_high_rtt_long_tail") {
            check(new_result.recovery_percent >= 20.0 &&
                      new_result.final_loss_percent <=
                          old_result.final_loss_percent * 0.80 &&
                      new_result.network_frame_drop_percent <=
                          old_result.network_frame_drop_percent * 0.80,
                  "180 ms cloud RTT must recover useful packets within the latency budget");
            check(new_result.presentation_miss_percent <=
                      old_result.presentation_miss_percent &&
                      new_result.longest_freeze_worst_ms <=
                          old_result.longest_freeze_worst_ms + 50,
                  "180 ms cloud RTT must bound the recovery latency trade-off");
        } else if (scenario.name == "cloud_very_high_rtt") {
            check(new_result.final_loss_percent <=
                      old_result.final_loss_percent + 0.01 &&
                      new_result.network_frame_drop_percent <=
                          old_result.network_frame_drop_percent + 0.01 &&
                      new_result.presentation_miss_percent <=
                          old_result.presentation_miss_percent + 0.01 &&
                      new_result.longest_freeze_worst_ms <=
                          old_result.longest_freeze_worst_ms + 1,
                  "240 ms cloud RTT must abandon unrecoverable HOL delay without regression");
        }
    }
    if (failures != 0) return 1;
    std::cout << "\nAll network strategy simulation checks passed\n";
    return 0;
}
