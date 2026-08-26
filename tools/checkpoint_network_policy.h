#pragma once

// Exact policy snapshot from the green8/G9 checkpoint. The production policy
// deliberately does not expose compatibility knobs; this test-only copy lets
// the deterministic network harness compare the checkpoint and current code
// on the same packet traces.

#include "webrtc/network_path_estimator.h"
#include "webrtc/video_jitter_policy.h"

#include <algorithm>
#include <cstdint>

namespace lunar::simulation {

inline webrtc::VideoJitterPolicy checkpointVideoJitterPolicy(
    webrtc::NetworkPathMode mode,
    const webrtc::NetworkPathEstimate& path) {
    const uint64_t rtt_ms = path.smoothed_rtt_ms > 0
        ? path.smoothed_rtt_ms : path.raw_rtt_ms;
    const uint64_t deadline_rtt_ms = std::max<uint64_t>(
        rtt_ms, path.raw_rtt_ms);
    const auto deadline_quality =
        static_cast<int>(path.observed_quality) >
                static_cast<int>(path.quality)
            ? path.observed_quality : path.quality;
    webrtc::VideoJitterPolicy policy;
    if (mode == webrtc::NetworkPathMode::Home) {
        policy.frame_hold_ms = std::clamp<uint64_t>(
            24 + rtt_ms / 4, 24, 48);
        const uint64_t margin_ms =
            deadline_quality == webrtc::NetworkPathQuality::Good ? 24 :
            deadline_quality == webrtc::NetworkPathQuality::Fair ? 40 : 60;
        const uint64_t cap_ms =
            deadline_quality == webrtc::NetworkPathQuality::Good ? 120 :
            deadline_quality == webrtc::NetworkPathQuality::Fair ? 160 : 200;
        policy.missing_packet_hold_ms = std::max<uint64_t>(
            policy.frame_hold_ms,
            std::clamp<uint64_t>(deadline_rtt_ms + margin_ms, 24, cap_ms));
        policy.recovery_hold_ms = std::clamp<uint64_t>(
            180 + deadline_rtt_ms / 2, 180, 250);
        policy.max_head_blocked_frames = 3;
        policy.head_blocked_hold_ms = std::min<uint64_t>(
            policy.missing_packet_hold_ms,
            std::clamp<uint64_t>(deadline_rtt_ms + 24, 32, 120));
        return policy;
    }

    policy.frame_hold_ms = std::clamp<uint64_t>(rtt_ms + 25, 80, 180);
    const uint64_t high_rtt_extra_ms = deadline_rtt_ms > 100
        ? deadline_rtt_ms - 100 : 0;
    const uint64_t margin_ms =
        deadline_quality == webrtc::NetworkPathQuality::Good ? 75 :
        deadline_quality == webrtc::NetworkPathQuality::Fair
            ? 115 + high_rtt_extra_ms / 2
            : 130 + high_rtt_extra_ms / 3;
    const uint64_t cap_ms =
        deadline_quality == webrtc::NetworkPathQuality::Good ? 320 :
        deadline_quality == webrtc::NetworkPathQuality::Fair ? 400 : 480;
    policy.missing_packet_hold_ms = std::max<uint64_t>(
        policy.frame_hold_ms,
        std::clamp<uint64_t>(deadline_rtt_ms + margin_ms, 80, cap_ms));
    policy.recovery_hold_ms = std::clamp<uint64_t>(
        180 + deadline_rtt_ms * 2, 300, 500);
    policy.max_head_blocked_frames = 6;
    const uint64_t head_latency_budget_ms =
        deadline_rtt_ms >= 200 ? 140 : 200;
    policy.head_blocked_hold_ms = std::min<uint64_t>(
        policy.missing_packet_hold_ms, head_latency_budget_ms);
    return policy;
}

class CheckpointAdaptiveBitrateController {
public:
    CheckpointAdaptiveBitrateController(webrtc::NetworkPathMode mode,
                                        int configured_max_kbps) {
        mode_ = mode;
        configured_max_kbps_ = std::max(1, configured_max_kbps);
        minimum_kbps_ = std::min(kFloorKbps, configured_max_kbps_);
        target_kbps_ = mode_ == webrtc::NetworkPathMode::Cloud
            ? std::min(kCloudStartupKbps, configured_max_kbps_)
            : configured_max_kbps_;
        congestion_episode_ceiling_kbps_ = target_kbps_;
    }

    int targetKbps() const { return target_kbps_; }

    int observe(const webrtc::NetworkPathEstimate& path) {
        if (!path.valid || path.sequence == 0 ||
            path.sequence == last_sequence_) {
            return target_kbps_;
        }
        last_sequence_ = path.sequence;

        const uint32_t congested_rtt_inflation =
            mode_ == webrtc::NetworkPathMode::Home
                ? kHomeCongestedRttInflationMs
                : kCloudCongestedRttInflationMs;
        const uint32_t stable_rtt_inflation =
            mode_ == webrtc::NetworkPathMode::Home
                ? kHomeStableRttInflationMs
                : kCloudStableRttInflationMs;
        const bool queue_congested = path.queue_depth >= kCongestedQueueDepth;
        const bool rtt_congested =
            path.rtt_inflation_ms >= congested_rtt_inflation;
        const bool severe = path.queue_drops > 0 ||
            path.unrecovered_loss_ppm >= kSevereLossPpm;
        const bool moderate_loss_congested =
            path.unrecovered_loss_ppm >= kCongestedLossPpm &&
            (queue_congested || rtt_congested);
        const bool strong_congestion = severe || moderate_loss_congested ||
            queue_congested;
        const bool rtt_only_congestion = rtt_congested && !strong_congestion;
        const bool congested = strong_congestion ||
            (rtt_only_congestion && !rtt_only_reduction_latched_);
        const bool stable = path.window_packets >= kMinimumPacketsPerWindow &&
            path.detected_loss_ppm < kStableDetectedLossPpm &&
            path.unrecovered_loss_ppm < kStableFinalLossPpm &&
            path.queue_drops == 0 &&
            path.queue_depth < kStableQueueDepth &&
            path.rtt_inflation_ms <= stable_rtt_inflation;

        if (strong_congestion) {
            rtt_only_reduction_latched_ = false;
            rtt_probe_windows_ = 0;
        } else if (rtt_only_reduction_latched_ && rtt_only_congestion) {
            if (path.rtt_inflation_ms + kRttProbeImprovementMs <=
                rtt_probe_best_inflation_ms_) {
                rtt_probe_best_inflation_ms_ = path.rtt_inflation_ms;
                rtt_probe_windows_ = 0;
            } else if (++rtt_probe_windows_ >= kRttProbeEvaluationWindows) {
                raiseOneStep();
                finishCongestionEpisodeIfRecovered();
                rtt_probe_windows_ = 0;
            }
            congested_windows_ = 0;
            stable_windows_ = 0;
            return target_kbps_;
        }

        if (severe) {
            const int previous_target = target_kbps_;
            lowerSeverely();
            recordCongestionReduction(previous_target);
            ever_congested_ = true;
            congested_windows_ = 0;
            stable_windows_ = 0;
        } else if (congested) {
            stable_windows_ = 0;
            const uint32_t windows_to_lower = rtt_only_congestion
                ? kRttOnlyWindowsToLower : kCongestedWindowsToLower;
            if (++congested_windows_ >= windows_to_lower) {
                const int previous_target = target_kbps_;
                lowerOneStep();
                recordCongestionReduction(previous_target);
                ever_congested_ = true;
                if (rtt_only_congestion) {
                    rtt_only_reduction_latched_ = true;
                    rtt_probe_windows_ = 0;
                    rtt_probe_best_inflation_ms_ = path.rtt_inflation_ms;
                }
                congested_windows_ = 0;
            }
        } else if (stable) {
            congested_windows_ = 0;
            rtt_only_reduction_latched_ = false;
            const uint32_t stable_windows_to_raise =
                congestion_episode_reductions_ == 1 &&
                        target_kbps_ < congestion_episode_ceiling_kbps_
                    ? kSingleEventStableWindowsToRaise
                    : mode_ == webrtc::NetworkPathMode::Cloud &&
                            !ever_congested_
                        ? kCloudStartupWindowsToRaise
                        : mode_ == webrtc::NetworkPathMode::Cloud
                            ? kCloudStableWindowsToRaise
                            : kHomeStableWindowsToRaise;
            if (++stable_windows_ >= stable_windows_to_raise) {
                raiseOneStep();
                finishCongestionEpisodeIfRecovered();
                stable_windows_ = 0;
            }
        } else {
            congested_windows_ = 0;
            if (stable_windows_ > 0) --stable_windows_;
        }
        return target_kbps_;
    }

private:
    static constexpr int kFloorKbps = 10000;
    static constexpr int kCloudStartupKbps = 20000;
    static constexpr int kStepKbps = 5000;
    static constexpr uint32_t kMinimumPacketsPerWindow = 200;
    static constexpr uint64_t kCongestedLossPpm = 3'000;
    static constexpr uint64_t kSevereLossPpm = 50'000;
    static constexpr uint64_t kStableDetectedLossPpm = 2'000;
    static constexpr uint64_t kStableFinalLossPpm = 500;
    static constexpr uint32_t kStableQueueDepth = 512;
    static constexpr uint32_t kCongestedQueueDepth = 1024;
    static constexpr uint32_t kHomeStableRttInflationMs = 12;
    static constexpr uint32_t kCloudStableRttInflationMs = 30;
    static constexpr uint32_t kHomeCongestedRttInflationMs = 25;
    static constexpr uint32_t kCloudCongestedRttInflationMs = 60;
    static constexpr uint32_t kCongestedWindowsToLower = 2;
    static constexpr uint32_t kRttOnlyWindowsToLower = 3;
    static constexpr uint32_t kRttProbeEvaluationWindows = 2;
    static constexpr uint32_t kRttProbeImprovementMs = 10;
    static constexpr uint32_t kCloudStartupWindowsToRaise = 2;
    static constexpr uint32_t kSingleEventStableWindowsToRaise = 2;
    static constexpr uint32_t kHomeStableWindowsToRaise = 5;
    static constexpr uint32_t kCloudStableWindowsToRaise = 6;

    void lowerOneStep() {
        if (target_kbps_ > minimum_kbps_) {
            target_kbps_ = std::max(minimum_kbps_, target_kbps_ - kStepKbps);
        }
    }

    void raiseOneStep() {
        if (target_kbps_ < configured_max_kbps_) {
            target_kbps_ = std::min(configured_max_kbps_,
                                    target_kbps_ + kStepKbps);
        }
    }

    void lowerSeverely() {
        if (target_kbps_ <= minimum_kbps_) return;
        const int reduced = target_kbps_ * 70 / 100;
        const int rounded = ((reduced + kStepKbps / 2) / kStepKbps) *
            kStepKbps;
        target_kbps_ = std::max(
            minimum_kbps_, std::min(target_kbps_ - kStepKbps, rounded));
    }

    void recordCongestionReduction(int previous_target_kbps) {
        if (target_kbps_ >= previous_target_kbps) return;
        if (congestion_episode_reductions_ == 0) {
            congestion_episode_ceiling_kbps_ = previous_target_kbps;
        }
        ++congestion_episode_reductions_;
    }

    void finishCongestionEpisodeIfRecovered() {
        if (congestion_episode_reductions_ == 0 ||
            target_kbps_ < congestion_episode_ceiling_kbps_) {
            return;
        }
        congestion_episode_reductions_ = 0;
        congestion_episode_ceiling_kbps_ = target_kbps_;
    }

    webrtc::NetworkPathMode mode_ = webrtc::NetworkPathMode::Home;
    int configured_max_kbps_ = kFloorKbps;
    int minimum_kbps_ = kFloorKbps;
    int target_kbps_ = kFloorKbps;
    uint32_t congested_windows_ = 0;
    uint32_t stable_windows_ = 0;
    bool ever_congested_ = false;
    uint32_t congestion_episode_reductions_ = 0;
    int congestion_episode_ceiling_kbps_ = kFloorKbps;
    bool rtt_only_reduction_latched_ = false;
    uint32_t rtt_probe_windows_ = 0;
    uint32_t rtt_probe_best_inflation_ms_ = 0;
    uint64_t last_sequence_ = 0;
};

} // namespace lunar::simulation
