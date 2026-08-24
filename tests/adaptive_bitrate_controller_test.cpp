#include "app/adaptive_bitrate_controller.h"
#include "webrtc/network_path_estimator.h"
#include "webrtc/video_jitter_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

using lunar::app::AdaptiveBitrateController;
using lunar::webrtc::NetworkPathEstimate;
using lunar::webrtc::NetworkPathEstimator;
using lunar::webrtc::NetworkPathMode;
using lunar::webrtc::NetworkPathQuality;
using lunar::webrtc::NetworkPathSample;
using lunar::webrtc::computeVideoJitterPolicy;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

NetworkPathSample nextSample(NetworkPathSample previous,
                             uint32_t packets,
                             uint32_t missing,
                             uint32_t recovered = 0,
                             uint32_t unrecovered = 0,
                             uint32_t queue_drops = 0,
                             uint32_t rtt_ms = 75) {
    previous.video_rtp_packets += packets;
    previous.video_payload_bytes += packets * 1000u;
    previous.video_missing_detected += missing;
    previous.video_missing_recovered += recovered;
    previous.video_missing_unrecovered += unrecovered;
    previous.rtp_queue_drops += queue_drops;
    previous.rtp_queue_depth = 0;
    previous.rtt_ms = rtt_ms;
    previous.interval_ms = 1000;
    return previous;
}

NetworkPathEstimate estimate(uint64_t sequence,
                             uint32_t packets = 2000,
                             uint64_t unrecovered_loss_ppm = 0,
                             uint32_t detected = 0,
                             uint32_t recovered = 0,
                             uint32_t queue_drops = 0,
                             uint32_t queue_depth = 0,
                             uint32_t smoothed_rtt_ms = 75,
                             uint32_t baseline_rtt_ms = 75,
                             uint32_t rtt_inflation_ms = 0,
                             uint32_t received_bitrate_kbps = 0,
                             uint32_t raw_rtt_ms = 0) {
    NetworkPathEstimate result;
    result.valid = true;
    result.sequence = sequence;
    result.window_packets = packets;
    result.detected_missing = detected;
    result.recovered_missing = recovered;
    const uint64_t total = static_cast<uint64_t>(packets) + detected;
    result.detected_loss_ppm = total == 0 ? 0 :
        static_cast<uint64_t>(detected) * 1'000'000u / total;
    result.unrecovered_loss_ppm = unrecovered_loss_ppm;
    result.queue_drops = queue_drops;
    result.queue_depth = queue_depth;
    result.smoothed_rtt_ms = smoothed_rtt_ms;
    result.raw_rtt_ms = raw_rtt_ms > 0 ? raw_rtt_ms : smoothed_rtt_ms;
    result.baseline_rtt_ms = baseline_rtt_ms;
    result.rtt_inflation_ms = rtt_inflation_ms;
    result.received_bitrate_kbps = received_bitrate_kbps;
    return result;
}

void test_path_estimator_accepts_stable_cloud_rtt() {
    NetworkPathEstimator estimator(NetworkPathMode::Cloud);
    NetworkPathSample sample;
    sample.rtt_ms = 75;
    auto path = estimator.observe(sample);
    require(!path.valid, "first path sample establishes a baseline");

    for (int i = 0; i < 4; ++i) {
        sample = nextSample(sample, 2000, 0, 0, 0, 0, 75);
        path = estimator.observe(sample);
    }
    require(path.valid, "cloud path estimate becomes valid");
    require(path.quality == NetworkPathQuality::Good,
            "stable 75 ms cloud RTT remains good");
    require(path.baseline_rtt_ms == 75 && path.rtt_inflation_ms == 0,
            "cloud RTT is evaluated relative to its baseline");
    require(path.received_bitrate_kbps == 16000,
            "path estimate reports measured inbound bitrate");
}

void test_path_estimator_detects_relative_rtt_inflation() {
    NetworkPathEstimator home(NetworkPathMode::Home);
    NetworkPathSample home_sample;
    home_sample.rtt_ms = 5;
    home.observe(home_sample);
    home_sample = nextSample(home_sample, 2000, 0, 0, 0, 0, 50);
    home.observe(home_sample);
    home_sample = nextSample(home_sample, 2000, 0, 0, 0, 0, 50);
    auto home_path = home.observe(home_sample);
    require(home_path.rtt_inflation_ms >= 40,
            "home path exposes latency inflation over LAN baseline");
    require(home_path.quality == NetworkPathQuality::Poor,
            "persistent LAN RTT inflation degrades path quality");

    NetworkPathEstimator cloud(NetworkPathMode::Cloud);
    NetworkPathSample cloud_sample;
    cloud_sample.rtt_ms = 70;
    cloud.observe(cloud_sample);
    cloud_sample = nextSample(cloud_sample, 2000, 0, 0, 0, 0, 150);
    cloud.observe(cloud_sample);
    cloud_sample = nextSample(cloud_sample, 2000, 0, 0, 0, 0, 150);
    auto cloud_path = cloud.observe(cloud_sample);
    require(cloud_path.rtt_inflation_ms >= 70,
            "cloud path detects a large rise over its own baseline");
    require(cloud_path.quality != NetworkPathQuality::Good,
            "inflated cloud RTT is no longer considered healthy");
}

void test_path_estimator_uses_deadline_loss_across_windows() {
    NetworkPathEstimator estimator(NetworkPathMode::Cloud);
    NetworkPathSample sample;
    sample.rtt_ms = 75;
    estimator.observe(sample);

    sample = nextSample(sample, 1980, 20);
    auto path = estimator.observe(sample);
    require(path.unrecovered_loss_ppm == 0,
            "new gaps are not treated as final loss before their deadline");

    sample = nextSample(sample, 2000, 0, 20);
    path = estimator.observe(sample);
    require(path.unrecovered_loss_ppm == 0,
            "next-window retransmissions remain recovered loss");

    sample = nextSample(sample, 1980, 20, 0, 20);
    path = estimator.observe(sample);
    require(path.unrecovered_loss_ppm >= 10'000,
            "packets abandoned at the frame deadline count as final loss");

    sample = nextSample(sample, 1980, 20, 20, 20);
    path = estimator.observe(sample);
    require(path.unrecovered_loss_ppm >= 10'000,
            "late recovery from another window cannot cancel new final loss");
}

void test_profile_specific_jitter_policy() {
    auto cloud_path = estimate(1, 2000, 0, 0, 0, 0, 0, 75, 75, 0);
    cloud_path.quality = NetworkPathQuality::Good;
    const auto cloud = computeVideoJitterPolicy(
        NetworkPathMode::Cloud, cloud_path);
    require(cloud.frame_hold_ms == 100,
            "stable cloud path uses bounded ordinary frame hold");
    require(cloud.missing_packet_hold_ms == 150,
            "cloud missing deadline is one RTT plus margin, not two RTTs");
    require(cloud.max_head_blocked_frames == 6,
            "cloud path tolerates a bounded frame backlog");

    auto high_rtt_path = estimate(3, 2000, 0, 0, 0, 0, 0,
                                  180, 180, 0);
    high_rtt_path.quality = NetworkPathQuality::Good;
    high_rtt_path.observed_quality = NetworkPathQuality::Good;
    const auto high_rtt = computeVideoJitterPolicy(
        NetworkPathMode::Cloud, high_rtt_path);
    require(high_rtt.missing_packet_hold_ms >= 230,
            "180 ms cloud path keeps one RTT plus retransmission margin");
    require(high_rtt.missing_packet_hold_ms < 300,
            "healthy high-RTT path remains below the legacy double-RTT hold");
    require(high_rtt.head_blocked_hold_ms == 200,
            "180 ms cloud RTT stays inside the usable A/V-sync budget");

    auto very_high_rtt_path = estimate(4, 2000, 20'000, 40, 0,
                                       0, 0, 240, 240, 0);
    very_high_rtt_path.quality = NetworkPathQuality::Poor;
    very_high_rtt_path.observed_quality = NetworkPathQuality::Poor;
    const auto very_high_rtt = computeVideoJitterPolicy(
        NetworkPathMode::Cloud, very_high_rtt_path);
    require(very_high_rtt.missing_packet_hold_ms >= 410 &&
                very_high_rtt.missing_packet_hold_ms < 440,
            "poor 240 ms path covers the retransmission tail below legacy hold");
    require(very_high_rtt.head_blocked_hold_ms == 140,
            "240 ms cloud RTT abandons HOL wait when one retry cannot be displayed");

    auto burst_path = estimate(5, 2000, 0, 60, 55, 0, 0,
                               85, 80, 5);
    burst_path.quality = NetworkPathQuality::Good;
    burst_path.observed_quality = NetworkPathQuality::Poor;
    const auto burst = computeVideoJitterPolicy(
        NetworkPathMode::Cloud, burst_path);
    require(burst.missing_packet_hold_ms >= 180,
            "an observed cloud burst expands the next recovery deadline immediately");

    auto home_path = estimate(2, 2000, 0, 0, 0, 0, 0, 5, 5, 0);
    home_path.quality = NetworkPathQuality::Good;
    const auto home = computeVideoJitterPolicy(NetworkPathMode::Home,
                                                home_path);
    require(home.frame_hold_ms <= 32 && home.missing_packet_hold_ms <= 32,
            "healthy LAN keeps retransmission latency below two frames");
    require(home.max_head_blocked_frames == 3,
            "home path prioritizes input-to-picture latency");

    auto home_50_path = estimate(6, 2000, 0, 0, 0, 0, 0,
                                 50, 50, 0);
    home_50_path.quality = NetworkPathQuality::Good;
    home_50_path.observed_quality = NetworkPathQuality::Good;
    const auto home_50 = computeVideoJitterPolicy(
        NetworkPathMode::Home, home_50_path);
    require(home_50.frame_hold_ms <= 40,
            "complete 50 ms LAN frames keep the low-latency pacing budget");
    require(home_50.missing_packet_hold_ms >= 70 &&
                home_50.missing_packet_hold_ms <= 80,
            "50 ms LAN loss leaves enough time for one retransmission");
    require(home_50.head_blocked_hold_ms ==
                home_50.missing_packet_hold_ms,
            "LAN HOL handling must not pre-empt its useful retransmission");

    auto sudden_home_rtt = estimate(7, 2000, 0, 20, 0, 0, 0,
                                    5, 5, 75, 0, 80);
    sudden_home_rtt.quality = NetworkPathQuality::Good;
    sudden_home_rtt.observed_quality = NetworkPathQuality::Poor;
    const auto home_spike = computeVideoJitterPolicy(
        NetworkPathMode::Home, sudden_home_rtt);
    require(home_spike.frame_hold_ms <= 32,
            "a raw RTT spike must not delay complete LAN frames");
    require(home_spike.missing_packet_hold_ms >= 140 &&
                home_spike.head_blocked_hold_ms >= 100,
            "a raw RTT spike with loss must immediately expand retry time");
}

} // namespace

int main() {
    test_path_estimator_accepts_stable_cloud_rtt();
    test_path_estimator_detects_relative_rtt_inflation();
    test_path_estimator_uses_deadline_loss_across_windows();
    test_profile_specific_jitter_policy();

    AdaptiveBitrateController controller(NetworkPathMode::Cloud, 30000);

    require(controller.targetKbps() == 20000,
            "cloud HQ starts conservatively at 20 Mbps");

    controller.observe(estimate(1));
    require(controller.targetKbps() == 20000,
            "one stable cloud window does not probe immediately");
    controller.observe(estimate(2));
    require(controller.targetKbps() == 25000,
            "cloud startup probes upward after two stable windows");
    controller.observe(estimate(3));
    controller.observe(estimate(4));
    require(controller.targetKbps() == 30000,
            "cloud startup reaches the user-selected cap");

    controller.observe(estimate(5, 2000, 10'000, 0, 0,
                                0, 0, 140, 75, 65));
    require(controller.targetKbps() == 30000,
            "one moderate congestion window is tolerated");
    controller.observe(estimate(6, 2000, 10'000, 0, 0,
                                0, 0, 140, 75, 65));
    require(controller.targetKbps() == 25000,
            "two moderate windows reduce one step");

    controller.observe(estimate(7, 2000, 60'000));
    require(controller.targetKbps() == 20000,
            "severe congestion applies a multiplicative decrease");

    for (uint64_t sequence = 8; sequence < 13; ++sequence) {
        controller.observe(estimate(sequence));
    }
    require(controller.targetKbps() == 20000,
            "post-congestion recovery waits for six clean windows");
    controller.observe(estimate(13));
    require(controller.targetKbps() == 25000,
            "stable cloud path restores one step after hysteresis");

    controller.observe(estimate(13));
    require(controller.targetKbps() == 25000,
            "duplicate estimator snapshots do not accelerate recovery");

    for (uint64_t sequence = 14; sequence < 20; ++sequence) {
        controller.observe(estimate(sequence));
    }
    require(controller.targetKbps() == 30000,
            "recovery reaches but never exceeds the configured cap");

    AdaptiveBitrateController recovered_loss(NetworkPathMode::Cloud, 20000);
    for (uint64_t sequence = 1; sequence <= 4; ++sequence) {
        recovered_loss.observe(estimate(sequence, 2000, 0, 20, 20));
    }
    require(recovered_loss.targetKbps() == 20000,
            "fully recovered packet loss does not force a downgrade");

    AdaptiveBitrateController queue_pressure(NetworkPathMode::Home, 20000);
    queue_pressure.observe(estimate(1, 2000, 0, 0, 0, 1));
    require(queue_pressure.targetKbps() == 15000,
            "local RTP queue drops cause an immediate downgrade");

    AdaptiveBitrateController low_cap(NetworkPathMode::Cloud, 10000);
    require(low_cap.targetKbps() == 10000, "720p cap starts at floor");
    for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
        low_cap.observe(estimate(sequence, 2000, 60'000));
    }
    require(low_cap.targetKbps() == 10000, "720p cap cannot fall below floor");

    AdaptiveBitrateController home_rtt(NetworkPathMode::Home, 20000);
    home_rtt.observe(estimate(1, 2000, 0, 0, 0, 0, 0, 45, 5, 40));
    home_rtt.observe(estimate(2, 2000, 0, 0, 0, 0, 0, 45, 5, 40));
    require(home_rtt.targetKbps() == 20000,
            "two RTT-only spikes are tolerated");
    home_rtt.observe(estimate(3, 2000, 0, 0, 0, 0, 0, 45, 5, 40));
    require(home_rtt.targetKbps() == 15000,
            "persistent relative LAN RTT inflation triggers one probe reduction");
    home_rtt.observe(estimate(4, 2000, 0, 0, 0, 0, 0, 45, 5, 40));
    home_rtt.observe(estimate(5, 2000, 0, 0, 0, 0, 0, 45, 5, 40));
    require(home_rtt.targetKbps() == 20000,
            "RTT-only probe rolls back when bitrate reduction does not improve RTT");
    for (uint64_t sequence = 6; sequence <= 12; ++sequence) {
        home_rtt.observe(estimate(sequence, 2000, 0, 0, 0,
                                  0, 0, 45, 5, 40));
    }
    require(home_rtt.targetKbps() == 20000,
            "persistent route RTT cannot repeatedly drive bitrate to the floor");

    AdaptiveBitrateController stable_wan(NetworkPathMode::Cloud, 30000);
    stable_wan.observe(estimate(1, 2000, 0, 0, 0, 0, 0, 120, 120, 0));
    stable_wan.observe(estimate(2, 2000, 0, 0, 0, 0, 0, 120, 120, 0));
    require(stable_wan.targetKbps() == 25000,
            "stable high absolute WAN RTT can still probe upward");

    AdaptiveBitrateController one_off_outage(NetworkPathMode::Cloud, 30000);
    for (uint64_t sequence = 1; sequence <= 4; ++sequence) {
        one_off_outage.observe(estimate(sequence));
    }
    require(one_off_outage.targetKbps() == 30000,
            "one-off test starts from the selected cloud cap");
    one_off_outage.observe(estimate(5, 2000, 600'000));
    require(one_off_outage.targetKbps() == 20000,
            "one severe window protects the stream immediately");
    one_off_outage.observe(estimate(6));
    one_off_outage.observe(estimate(7));
    require(one_off_outage.targetKbps() == 25000,
            "single-window congestion recovers one tier after two clean windows");
    one_off_outage.observe(estimate(8));
    one_off_outage.observe(estimate(9));
    require(one_off_outage.targetKbps() == 30000,
            "single-window congestion quickly restores its pre-event cap");

    AdaptiveBitrateController mild_recovered(NetworkPathMode::Cloud, 30000);
    mild_recovered.observe(estimate(1, 2000, 60'000, 120, 0, 1));
    require(mild_recovered.targetKbps() == 15000,
            "receiver queue loss still causes an immediate reduction");
    for (uint64_t sequence = 2; sequence <= 3; ++sequence) {
        mild_recovered.observe(estimate(sequence, 2000, 0, 2, 2,
                                        0, 0, 80, 80, 0, 15000));
    }
    require(mild_recovered.targetKbps() == 20000,
            "small fully recovered loss permits post-congestion recovery");

    AdaptiveBitrateController random_loss(NetworkPathMode::Home, 20000);
    random_loss.observe(estimate(1, 2000, 4'000, 8, 0,
                                 0, 0, 8, 8, 0, 19900));
    random_loss.observe(estimate(2, 2000, 4'000, 8, 0,
                                 0, 0, 8, 8, 0, 19900));
    require(random_loss.targetKbps() == 20000,
            "loss without throughput, queue, or RTT congestion evidence does not downshift");

    AdaptiveBitrateController capacity_limited(NetworkPathMode::Home, 20000);
    capacity_limited.observe(estimate(1, 2000, 4'000, 8, 0,
                                      0, 0, 45, 5, 40, 15000));
    capacity_limited.observe(estimate(2, 2000, 4'000, 8, 0,
                                      0, 0, 45, 5, 40, 15000));
    require(capacity_limited.targetKbps() == 15000,
            "loss with measured throughput and RTT congestion still downshifts");

    std::cout << "Adaptive bitrate controller tests passed\n";
    return 0;
}
