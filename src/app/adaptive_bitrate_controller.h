#pragma once

#include "../webrtc/network_path_estimator.h"

#include <algorithm>
#include <cstdint>

namespace lunar::app {

struct AdaptiveBitrateSignal {
    int requested_width = 0;
    int requested_height = 0;
    int decoded_width = 0;
    int decoded_height = 0;
    bool hard_recovery = false;
};

// Receiver-side REMB controller for Xbox streams. NetworkPathEstimator owns
// counter deltas, RTT baselining, and loss finalisation; this controller only
// maps that shared estimate onto a bitrate target.
class AdaptiveBitrateController {
public:
    AdaptiveBitrateController(webrtc::NetworkPathMode mode,
                              int configured_max_kbps) {
        reset(mode, configured_max_kbps);
    }

    void reset(webrtc::NetworkPathMode mode, int configured_max_kbps) {
        mode_ = mode;
        configured_max_kbps_ = std::max(1, configured_max_kbps);
        minimum_kbps_ = std::min(kFloorKbps, configured_max_kbps_);
        // Home links normally have ample headroom and should start at the
        // selected cap. Cloud HQ starts at the ordinary 20 Mbps tier and
        // probes upward after two clean windows to avoid a rough startup.
        target_kbps_ = mode_ == webrtc::NetworkPathMode::Cloud
            ? std::min(kCloudStartupKbps, configured_max_kbps_)
            : configured_max_kbps_;
        congested_windows_ = 0;
        stable_windows_ = 0;
        ever_congested_ = false;
        congestion_episode_reductions_ = 0;
        congestion_episode_ceiling_kbps_ = target_kbps_;
        rtt_only_reduction_latched_ = false;
        rtt_probe_windows_ = 0;
        rtt_probe_best_inflation_ms_ = 0;
        recovery_cooldown_windows_ = 0;
        receiver_ceiling_kbps_ = configured_max_kbps_;
        oversized_stream_latched_ = false;
        last_sequence_ = 0;
    }

    int targetKbps() const { return target_kbps_; }

    int observe(const webrtc::NetworkPathEstimate& path) {
        return observe(path, {});
    }

    int observe(const webrtc::NetworkPathEstimate& path,
                const AdaptiveBitrateSignal& signal) {
        // hard_recovery is an edge event from decoder/keyframe counters and
        // may arrive between estimator windows. Receiver capability signals
        // are idempotent, so both must be handled before path de-duplication.
        applyReceiverSignal(signal);
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
        const bool queue_congested =
            path.queue_depth >= kCongestedQueueDepth;
        const bool rtt_congested =
            path.rtt_inflation_ms >= congested_rtt_inflation;
        const bool severe = path.queue_drops > 0 ||
            path.unrecovered_loss_ppm >= kSevereLossPpm;
        // Moderate isolated loss can be RF interference or internet route
        // loss rather than sender congestion. Lower only when queue growth or
        // RTT inflation corroborates it; severe final loss remains actionable
        // on its own.
        const bool moderate_loss_congested =
            path.unrecovered_loss_ppm >= kCongestedLossPpm &&
            (queue_congested || rtt_congested);
        const bool strong_congestion = severe || moderate_loss_congested ||
            queue_congested;
        const bool rtt_only_congestion = rtt_congested &&
            !strong_congestion;
        const bool congested = strong_congestion ||
            (rtt_only_congestion && !rtt_only_reduction_latched_);
        // A few repaired packets are normal on Wi-Fi/WAN and must not pin a
        // recovered stream at the floor forever. Stable absolute WAN RTT is
        // acceptable; congestion is RTT inflation over the path baseline.
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
            // The one-step RTT-only reduction is a probe. If two following
            // windows show no material RTT improvement, this is more likely a
            // route/base-latency shift than bitrate congestion; roll it back
            // and suppress another probe until RTT returns to baseline.
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

        if (signal.hard_recovery) {
            // applyReceiverSignal() already performed the one-step protective
            // reduction. Do not immediately apply a second multiplicative
            // decrease for counters from the same recovery episode.
            congested_windows_ = 0;
            stable_windows_ = 0;
        } else if (severe) {
            const int previous_target = target_kbps_;
            lowerSeverely();
            recordCongestionReduction(previous_target);
            if (mode_ == webrtc::NetworkPathMode::Cloud) {
                recovery_cooldown_windows_ = std::max(
                    recovery_cooldown_windows_, kCloudSevereCooldownWindows);
            }
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
                if (mode_ == webrtc::NetworkPathMode::Cloud) {
                    recovery_cooldown_windows_ = std::max(
                        recovery_cooldown_windows_,
                        kCloudCongestionCooldownWindows);
                }
                ever_congested_ = true;
                if (rtt_only_congestion) {
                    rtt_only_reduction_latched_ = true;
                    rtt_probe_windows_ = 0;
                    rtt_probe_best_inflation_ms_ =
                        path.rtt_inflation_ms;
                }
                congested_windows_ = 0;
            }
        } else if (stable) {
            congested_windows_ = 0;
            rtt_only_reduction_latched_ = false;
            if (recovery_cooldown_windows_ > 0) {
                --recovery_cooldown_windows_;
                stable_windows_ = 0;
                return target_kbps_;
            }
            const uint32_t stable_windows_to_raise =
                congestion_episode_reductions_ == 1 &&
                        target_kbps_ < congestion_episode_ceiling_kbps_
                    ? mode_ == webrtc::NetworkPathMode::Cloud
                        ? kCloudSingleEventStableWindowsToRaise
                        : kHomeSingleEventStableWindowsToRaise
                    : mode_ == webrtc::NetworkPathMode::Cloud && !ever_congested_
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
            // A neutral window (for example one isolated recovered Wi-Fi
            // gap) should delay an increase, not erase all prior evidence.
            // Repeated neutral windows still drain the recovery score.
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
    static constexpr uint32_t kHomeSingleEventStableWindowsToRaise = 2;
    static constexpr uint32_t kCloudSingleEventStableWindowsToRaise = 2;
    static constexpr uint32_t kHomeStableWindowsToRaise = 5;
    static constexpr uint32_t kCloudStableWindowsToRaise = 6;
    static constexpr uint32_t kCloudCongestionCooldownWindows = 1;
    static constexpr uint32_t kCloudSevereCooldownWindows = 1;
    static constexpr uint32_t kHomeHardRecoveryCooldownWindows = 2;
    static constexpr uint32_t kCloudHardRecoveryCooldownWindows = 15;
    static constexpr int kOversizedCloudCeilingKbps = 20000;

    void applyReceiverSignal(const AdaptiveBitrateSignal& signal) {
        if (mode_ == webrtc::NetworkPathMode::Cloud &&
            signal.requested_width > 0 && signal.requested_height > 0 &&
            signal.decoded_width > 0 && signal.decoded_height > 0) {
            const uint64_t requested_pixels =
                static_cast<uint64_t>(signal.requested_width) *
                static_cast<uint64_t>(signal.requested_height);
            const uint64_t decoded_pixels =
                static_cast<uint64_t>(signal.decoded_width) *
                static_cast<uint64_t>(signal.decoded_height);
            if (decoded_pixels > requested_pixels) {
                oversized_stream_latched_ = true;
                receiver_ceiling_kbps_ = std::min(
                    configured_max_kbps_, kOversizedCloudCeilingKbps);
                if (target_kbps_ > receiver_ceiling_kbps_) {
                    const int previous_target = target_kbps_;
                    target_kbps_ = receiver_ceiling_kbps_;
                    recordCongestionReduction(previous_target);
                    ever_congested_ = true;
                    stable_windows_ = 0;
                }
            }
        }

        if (!signal.hard_recovery) return;
        const int previous_target = target_kbps_;
        lowerOneStep();
        recordCongestionReduction(previous_target);
        ever_congested_ = true;
        congested_windows_ = 0;
        stable_windows_ = 0;
        recovery_cooldown_windows_ = std::max(
            recovery_cooldown_windows_,
            mode_ == webrtc::NetworkPathMode::Cloud
                ? kCloudHardRecoveryCooldownWindows
                : kHomeHardRecoveryCooldownWindows);
    }

    void lowerOneStep() {
        if (target_kbps_ <= minimum_kbps_) return;
        target_kbps_ = std::max(minimum_kbps_, target_kbps_ - kStepKbps);
    }

    void raiseOneStep() {
        const int ceiling_kbps = std::min(configured_max_kbps_,
                                          receiver_ceiling_kbps_);
        if (target_kbps_ >= ceiling_kbps) return;
        target_kbps_ = std::min(ceiling_kbps,
                                target_kbps_ + kStepKbps);
    }

    void lowerSeverely() {
        if (target_kbps_ <= minimum_kbps_) return;
        const int reduced = target_kbps_ * 70 / 100;
        const int rounded = ((reduced + kStepKbps / 2) / kStepKbps) *
            kStepKbps;
        target_kbps_ = std::max(minimum_kbps_,
                                std::min(target_kbps_ - kStepKbps, rounded));
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
    uint32_t recovery_cooldown_windows_ = 0;
    int receiver_ceiling_kbps_ = kFloorKbps;
    bool oversized_stream_latched_ = false;
    uint64_t last_sequence_ = 0;
};

} // namespace lunar::app
