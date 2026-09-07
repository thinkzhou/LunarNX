#pragma once

#include "network_path_estimator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace lunar::webrtc {

struct VideoJitterPolicy {
    uint64_t frame_hold_ms = 24;
    uint64_t missing_packet_hold_ms = 32;
    uint64_t recovery_hold_ms = 180;
    size_t max_head_blocked_frames = 3;
    uint64_t head_blocked_hold_ms = 32;
};

inline VideoJitterPolicy computeVideoJitterPolicy(
    NetworkPathMode mode,
    const NetworkPathEstimate& path) {
    const uint64_t rtt_ms = path.smoothed_rtt_ms > 0
        ? path.smoothed_rtt_ms : path.raw_rtt_ms;
    // Frame pacing follows the smoothed path, but a missing-packet deadline
    // must not lag behind a sudden RTT rise by several EWMA windows. A larger
    // raw sample only matters when a packet is already missing, so reacting
    // immediately does not add latency to complete frames.
    const uint64_t deadline_rtt_ms = std::max<uint64_t>(
        rtt_ms, path.raw_rtt_ms);
    // Quality reporting is intentionally hysteretic, but a recovery deadline
    // must react to the first bad window. Use the worse of the stable and
    // immediately observed qualities for the deadline only.
    const NetworkPathQuality deadline_quality =
        static_cast<int>(path.observed_quality) >
                static_cast<int>(path.quality)
            ? path.observed_quality : path.quality;
    VideoJitterPolicy policy;
    if (mode == NetworkPathMode::Home) {
        // Home includes remote consoles over the internet, not just LAN.
        // Restore v0.2.0's bounded recovery/queued-frame budgets: the newer
        // 120 ms HOL cap could discard a frame before its WAN retransmission.
        // Complete frames still emit immediately; these are loss deadlines,
        // not prebuffer targets. Use raw RTT on spikes as well as the EWMA.
        const uint64_t bounded_rtt_ms = std::min<uint64_t>(deadline_rtt_ms, 2000);
        policy.frame_hold_ms = bounded_rtt_ms > 0
            ? std::clamp<uint64_t>(bounded_rtt_ms * 2 + 20, 60, 180)
            : 120;
        policy.missing_packet_hold_ms = policy.frame_hold_ms;
        policy.recovery_hold_ms = std::clamp<uint64_t>(
            bounded_rtt_ms + 150, 300, 800);
        policy.max_head_blocked_frames = 8;
        // drain() caps this against the current frame's deadline. Do not
        // pre-cap it against an ordinary P-frame: recovery IDRs have a longer
        // budget and must retain it even when later frames accumulate.
        policy.head_blocked_hold_ms = std::max<uint64_t>(80, bounded_rtt_ms + 20);
        return policy;
    }

    // Complete frames bypass these deadlines. Keep the no-marker/no-NACK
    // timeout small so an incomplete head cannot add a full RTT before we
    // even know which packet to repair.
    policy.frame_hold_ms = std::clamp<uint64_t>(
        40 + rtt_ms / 4, 50, 100);
    const uint64_t margin_ms =
        deadline_quality == NetworkPathQuality::Good ? 65 :
        deadline_quality == NetworkPathQuality::Fair ? 75 : 100;
    const uint64_t cap_ms =
        deadline_quality == NetworkPathQuality::Good ? 240 :
        deadline_quality == NetworkPathQuality::Fair ? 300 : 400;
    policy.missing_packet_hold_ms = std::max<uint64_t>(
        policy.frame_hold_ms,
        std::clamp<uint64_t>(deadline_rtt_ms + margin_ms, 60, cap_ms));
    policy.recovery_hold_ms = std::clamp<uint64_t>(
        200 + deadline_rtt_ms, 280, 480);
    policy.max_head_blocked_frames = 3;
    // Normal cloud paths get a much tighter HOL budget. High-RTT paths retain
    // enough time for one useful retransmission instead of timing out before
    // a packet can physically make the round trip.
    const uint64_t head_latency_budget_ms =
        deadline_rtt_ms >= 220
            ? 140
            : deadline_rtt_ms >= 150
                ? std::min<uint64_t>(220, deadline_rtt_ms + 40)
                : deadline_quality == NetworkPathQuality::Good ? 160
                : deadline_quality == NetworkPathQuality::Fair ? 170 : 190;
    policy.head_blocked_hold_ms = std::min<uint64_t>(
        policy.missing_packet_hold_ms, head_latency_budget_ms);
    return policy;
}

} // namespace lunar::webrtc
