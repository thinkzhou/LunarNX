#include "webrtc/video_rtp_jitter_buffer.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using lunar::webrtc::VideoRtpJitterBuffer;

std::vector<uint8_t> rtp(uint16_t sequence,
                         uint32_t timestamp,
                         bool marker,
                         const std::vector<uint8_t>& payload,
                         size_t padding = 0,
                         uint32_t ssrc = 1) {
    std::vector<uint8_t> packet(12 + payload.size() + padding, 0);
    packet[0] = static_cast<uint8_t>(0x80 | (padding ? 0x20 : 0));
    packet[1] = static_cast<uint8_t>(102 | (marker ? 0x80 : 0));
    packet[2] = static_cast<uint8_t>(sequence >> 8);
    packet[3] = static_cast<uint8_t>(sequence);
    packet[4] = static_cast<uint8_t>(timestamp >> 24);
    packet[5] = static_cast<uint8_t>(timestamp >> 16);
    packet[6] = static_cast<uint8_t>(timestamp >> 8);
    packet[7] = static_cast<uint8_t>(timestamp);
    packet[8] = static_cast<uint8_t>(ssrc >> 24);
    packet[9] = static_cast<uint8_t>(ssrc >> 16);
    packet[10] = static_cast<uint8_t>(ssrc >> 8);
    packet[11] = static_cast<uint8_t>(ssrc);
    std::memcpy(packet.data() + 12, payload.data(), payload.size());
    if (padding) packet.back() = static_cast<uint8_t>(padding);
    return packet;
}

struct Harness {
    VideoRtpJitterBuffer jitter;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<std::pair<uint16_t, uint16_t>> nacks;
    std::vector<uint32_t> source_discontinuities;
    int recovery_requests = 0;
    int decoder_resets = 0;

    void push(const std::vector<uint8_t>& packet, uint64_t now_ms) {
        jitter.receive(
            packet.data(), packet.size(), now_ms,
            [this](const uint8_t* data, size_t size, uint16_t, uint32_t) {
                frames.emplace_back(data, data + size);
            },
            [this](uint16_t pid, uint16_t blp) {
                nacks.emplace_back(pid, blp);
                return true;
            },
            [this](bool reset_decoder) {
                recovery_requests++;
                if (reset_decoder) decoder_resets++;
            },
            [this](uint32_t ssrc) {
                source_discontinuities.push_back(ssrc);
            });
    }
};

void openWithIdr(Harness& h, uint16_t sequence, uint32_t timestamp) {
    h.push(rtp(sequence, timestamp, true, {0x65, 0xaa}), 0);
    assert(h.frames.size() == 1);
    assert(!h.jitter.waitingForKeyframe());
}

void test_reorders_retransmitted_fu_a() {
    Harness h;
    openWithIdr(h, 10, 1000);

    h.push(rtp(11, 2000, false, {0x7c, 0x81, 0x11}), 1);
    h.push(rtp(13, 2000, true, {0x7c, 0x41, 0x33}), 2);
    assert(h.frames.size() == 1);
    assert(h.nacks.size() == 1);
    assert(h.nacks[0].first == 12);

    h.push(rtp(12, 2000, false, {0x7c, 0x01, 0x22}), 3);
    assert(h.frames.size() == 2);
    const std::vector<uint8_t> expected = {
        0x00, 0x00, 0x00, 0x01, 0x61, 0x11, 0x22, 0x33,
    };
    assert(h.frames.back() == expected);
    assert(h.jitter.stats().missing_packets == 0);
}

void test_repeated_timeouts_gate_p_frames_until_real_idr() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 100, 1000);

    h.push(rtp(101, 2000, false, {0x7c, 0x81, 0x11}), 1);
    h.push(rtp(103, 2000, true, {0x7c, 0x41, 0x33}), 2);
    h.push(rtp(104, 3000, true, {0x61, 0x44}), 60);

    assert(h.frames.size() == 2);
    assert(!h.jitter.waitingForKeyframe());
    assert(h.decoder_resets == 0);

    h.push(rtp(105, 4000, false, {0x7c, 0x81, 0x55}), 61);
    h.push(rtp(107, 4000, true, {0x7c, 0x41, 0x77}), 62);
    h.push(rtp(108, 5000, true, {0x61, 0x88}), 120);

    assert(h.frames.size() == 2);
    assert(h.jitter.waitingForKeyframe());
    assert(h.decoder_resets == 0);
    assert(h.jitter.stats().corrupt_frames == 2);

    h.push(rtp(109, 6000, true, {0x67, 0x42}), 121);
    assert(h.frames.size() == 2);
    h.push(rtp(110, 7000, true, {0x65, 0x55}), 122);
    assert(h.frames.size() == 3);
    assert(!h.jitter.waitingForKeyframe());
}

void test_active_frame_progress_extends_idle_deadline() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 120, 1000);

    h.push(rtp(121, 2000, false, {0x7c, 0x81, 0x11}), 1);
    h.push(rtp(122, 2000, false, {0x7c, 0x01, 0x22}), 40);
    h.push(rtp(123, 2000, false, {0x7c, 0x01, 0x33}), 80);
    assert(h.jitter.stats().corrupt_frames == 0);
    assert(h.frames.size() == 1);

    h.push(rtp(124, 2000, true, {0x7c, 0x41, 0x44}), 120);
    assert(h.jitter.stats().corrupt_frames == 0);
    assert(h.frames.size() == 2);
    assert(!h.jitter.waitingForKeyframe());
}

void test_active_frame_progress_survives_old_hard_deadline() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 140, 1000);

    h.push(rtp(141, 2000, false, {0x7c, 0x81, 0x11}), 1);
    for (uint16_t sequence = 142; sequence <= 148; ++sequence) {
        h.push(rtp(sequence, 2000, false, {0x7c, 0x01, 0x22}),
               40 * (sequence - 140));
    }

    assert(h.jitter.stats().corrupt_frames == 0);
    assert(h.jitter.stats().buffered_frames == 1);
    assert(h.decoder_resets == 0);

    h.push(rtp(149, 2000, true, {0x7c, 0x41, 0x33}), 330);
    assert(h.frames.size() == 2);
    assert(h.jitter.stats().corrupt_frames == 0);
    assert(!h.jitter.waitingForKeyframe());
}

void test_pathological_active_frame_obeys_safety_cap_without_decoder_reset() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 150, 1000);

    h.push(rtp(151, 2000, false, {0x7c, 0x81, 0x11}), 1);
    for (uint16_t sequence = 152; sequence <= 162; ++sequence) {
        h.push(rtp(sequence, 2000, false, {0x7c, 0x01, 0x22}),
               100 * (sequence - 151));
    }

    assert(h.jitter.stats().corrupt_frames == 1);
    assert(h.jitter.stats().buffered_frames == 0);
    assert(h.jitter.waitingForKeyframe());
    assert(h.recovery_requests == 1);
    assert(h.decoder_resets == 0);
}

void test_isolated_p_frame_timeout_uses_soft_recovery() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 160, 1000);

    h.push(rtp(161, 2000, false, {0x7c, 0x81, 0x11}), 1);
    h.push(rtp(162, 3000, true, {0x61, 0x22}), 60);

    assert(h.jitter.stats().corrupt_frames == 1);
    assert(h.frames.size() == 2);
    assert(!h.jitter.waitingForKeyframe());
    assert(h.recovery_requests == 1);
    assert(h.decoder_resets == 0);
}

void test_incomplete_idr_waits_for_recovery_without_decoder_reset() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 180, 1000);

    h.push(rtp(181, 2000, false, {0x7c, 0x85, 0x11}), 1);
    h.push(rtp(182, 3000, true, {0x61, 0x22}), 60);

    assert(h.jitter.stats().corrupt_frames == 1);
    assert(h.frames.size() == 1);
    assert(h.jitter.waitingForKeyframe());
    assert(h.recovery_requests == 1);
    assert(h.decoder_resets == 0);
}

void test_padding_consumes_sequence_without_false_loss() {
    Harness h;
    openWithIdr(h, 200, 1000);

    h.push(rtp(201, 1000, false, {}, 8), 1);
    h.push(rtp(202, 2000, true, {0x61, 0x22}), 2);

    assert(h.frames.size() == 2);
    assert(h.nacks.empty());
    assert(h.jitter.stats().missing_packets == 0);
}

void test_late_packet_repairs_receiver_report_loss() {
    Harness h;
    openWithIdr(h, 300, 1000);
    h.push(rtp(302, 2000, true, {0x61, 0x22}), 1);
    auto before = h.jitter.receiverReport();
    assert(before.cumulative_lost == 1);

    h.push(rtp(301, 2000, false, {0x61, 0x11}), 2);
    auto after = h.jitter.receiverReport();
    assert(after.cumulative_lost == 0);
}

void test_missing_detection_totals_do_not_roll_back_after_recovery() {
    Harness h;
    openWithIdr(h, 340, 1000);

    h.push(rtp(342, 2000, true, {0x61, 0x22}), 1);
    auto missing = h.jitter.stats();
    assert(missing.missing_packets == 1);
    assert(missing.missing_packets_detected == 1);
    assert(missing.missing_packets_recovered == 0);

    h.push(rtp(341, 2000, false, {0x61, 0x11}), 2);
    auto recovered = h.jitter.stats();
    assert(recovered.missing_packets == 0);
    assert(recovered.missing_packets_detected == 1);
    assert(recovered.missing_packets_recovered == 1);

    h.push(rtp(344, 3000, true, {0x61, 0x44}), 3);
    auto second_gap = h.jitter.stats();
    assert(second_gap.missing_packets == 1);
    assert(second_gap.missing_packets_detected == 2);
    assert(second_gap.missing_packets_recovered == 1);
}

void test_packet_before_initial_sequence_is_not_reported_as_recovered() {
    Harness h;
    openWithIdr(h, 400, 1000);

    h.push(rtp(399, 1000, false, {0x67, 0x42}), 1);

    const auto stats = h.jitter.stats();
    assert(stats.missing_packets == 0);
    assert(stats.missing_packets_detected == 0);
    assert(stats.missing_packets_recovered == 0);
    assert(h.jitter.receiverReport().cumulative_lost == 0);

    h.push(rtp(402, 2000, true, {0x61, 0x22}), 2);
    const auto after_gap = h.jitter.stats();
    assert(after_gap.missing_packets == 1);
    assert(after_gap.missing_packets_detected == 1);
    assert(after_gap.missing_packets_recovered == 0);
    assert(h.jitter.receiverReport().cumulative_lost == 1);
}

void test_tracks_arrival_gap_sequence_jump_and_ssrc_change() {
    Harness h;
    h.push(rtp(10, 1000, true, {0x65, 0x11}, 0, 0x11223344), 100);
    h.push(rtp(12, 2000, true, {0x61, 0x22}, 0, 0x11223344), 150);

    auto stats = h.jitter.stats();
    assert(stats.last_gap_packets == 1);
    assert(stats.last_arrival_gap_ms == 50);
    assert(stats.max_arrival_gap_ms == 50);
    assert(stats.ssrc == 0x11223344);
    assert(stats.ssrc_changes == 0);

    h.push(rtp(13, 3000, true, {0x65, 0x33}, 0, 0xaabbccdd), 180);
    stats = h.jitter.stats();
    assert(stats.last_arrival_gap_ms == 30);
    assert(stats.max_arrival_gap_ms == 50);
    assert(stats.ssrc == 0xaabbccdd);
    assert(stats.ssrc_changes == 1);
    assert(h.source_discontinuities.size() == 1);
    assert(h.source_discontinuities[0] == 0xaabbccdd);
}

void test_timestamp_restart_resets_source_state() {
    Harness h;
    openWithIdr(h, 200, 900000);
    h.push(rtp(201, 901500, true, {0x61, 0x22}), 10);
    h.push(rtp(202, 1000, true, {0x65, 0x33}), 20);

    const auto stats = h.jitter.stats();
    assert(stats.timestamp_discontinuities == 1);
    assert(h.source_discontinuities.size() == 1);
    assert(h.source_discontinuities[0] == 1);
    assert(!h.jitter.waitingForKeyframe());
}

void test_stale_retransmission_does_not_clear_recovery_state() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 320, 1000);

    h.push(rtp(321, 2000, false, {0x7c, 0x85, 0x11}), 1);
    h.push(rtp(323, 2000, true, {0x7c, 0x45, 0x33}), 2);
    h.push(rtp(324, 3000, true, {0x61, 0x44}), 60);
    assert(h.jitter.waitingForKeyframe());
    const auto overflow_before = h.jitter.stats().overflow_frames;

    h.push(rtp(322, 2000, false, {0x7c, 0x05, 0x22}), 61);
    assert(h.jitter.stats().overflow_frames == overflow_before);
    assert(h.jitter.stats().missing_packets == 0);

    h.push(rtp(325, 4000, true, {0x65, 0x55}), 62);
    assert(h.frames.size() == 2);
    assert(!h.jitter.waitingForKeyframe());
}

void test_missing_marker_does_not_block_later_idr() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 340, 1000);

    h.push(rtp(341, 2000, false, {0x65, 0x11}), 1);
    h.push(rtp(343, 3000, true, {0x61, 0x22}), 60);
    assert(h.jitter.waitingForKeyframe());
    assert(h.frames.size() == 1);

    h.push(rtp(344, 4000, true, {0x65, 0x33}), 61);
    assert(h.frames.size() == 2);
    assert(!h.jitter.waitingForKeyframe());
}

void test_sequence_wrap_reorders_retransmission() {
    Harness h;
    openWithIdr(h, 0xfffe, 1000);

    h.push(rtp(0xffff, 2000, false, {0x7c, 0x81, 0x11}), 1);
    h.push(rtp(1, 2000, true, {0x7c, 0x41, 0x33}), 2);
    assert(h.nacks.size() == 1);
    assert(h.nacks[0].first == 0);
    assert(h.nacks[0].second == 0);

    h.push(rtp(0, 2000, false, {0x7c, 0x01, 0x22}), 3);
    assert(h.frames.size() == 2);
    assert(h.jitter.stats().missing_packets == 0);
    assert(h.jitter.stats().highest_sequence == 0x10001u);
}

void test_malformed_payload_is_not_reported_as_overflow() {
    Harness h;
    openWithIdr(h, 360, 1000);

    h.push(rtp(361, 2000, true, {0x78, 0x00, 0x05, 0x67}), 1);
    const auto stap_stats = h.jitter.stats();
    assert(stap_stats.corrupt_frames == 1);
    assert(stap_stats.overflow_frames == 0);
    assert(h.jitter.waitingForKeyframe());
    assert(h.decoder_resets == 1);

    h.push(rtp(362, 3000, true, {0x65, 0x22}), 2);
    assert(!h.jitter.waitingForKeyframe());
    h.push(rtp(363, 4000, true, {0x7c, 0x81, 0x33}), 3);
    const auto fu_stats = h.jitter.stats();
    assert(fu_stats.corrupt_frames == 2);
    assert(fu_stats.overflow_frames == 0);
    assert(h.decoder_resets == 2);
}

void test_access_unit_limit_is_enforced_without_unbounded_growth() {
    Harness h;
    openWithIdr(h, 380, 1000);
    std::vector<uint8_t> payload(1900, 0x11);
    payload[0] = 0x61;

    constexpr size_t packet_count = 1105;
    for (size_t i = 0; i < packet_count; ++i) {
        h.push(rtp(static_cast<uint16_t>(381 + i),
                   2000,
                   i + 1 == packet_count,
                   payload),
               1);
    }
    const auto stats = h.jitter.stats();
    assert(stats.overflow_frames == 1);
    assert(stats.corrupt_frames == 1);
    assert(stats.buffered_bytes == 0);
    assert(stats.buffered_packets == 0);
    assert(stats.buffered_frames == 0);
    assert(h.jitter.waitingForKeyframe());
    assert(h.decoder_resets == 1);
}

void test_buffer_caps_are_hard_limits() {
    Harness h;
    openWithIdr(h, 400, 1000);
    std::vector<uint8_t> payload(1800, 0x11);
    payload[0] = 0x61;

    for (uint32_t i = 0; i < 3000; ++i) {
        h.push(rtp(static_cast<uint16_t>(401 + i),
                   2000 + i * 3000,
                   false,
                   payload),
               i + 1);
        const auto stats = h.jitter.stats();
        assert(stats.buffered_frames <= VideoRtpJitterBuffer::kMaxBufferedFrames);
        assert(stats.buffered_packets <= VideoRtpJitterBuffer::kMaxBufferedPackets);
        assert(stats.buffered_bytes <= VideoRtpJitterBuffer::kMaxBufferedBytes);
    }
    assert(h.jitter.stats().overflow_frames > 0);
}

void test_large_frame_defers_assembly_and_reuses_payload_storage() {
    Harness h;
    openWithIdr(h, 1000, 1000);
    const auto baseline = h.jitter.stats();

    std::vector<uint8_t> payload(1000, 0x11);
    payload[0] = 0x61;
    constexpr size_t packet_count = 512;
    for (size_t i = 0; i < packet_count; ++i) {
        h.push(rtp(static_cast<uint16_t>(1001 + i),
                   2000,
                   false,
                   payload),
               1);
    }

    const auto buffered = h.jitter.stats();
    assert(buffered.assembly_attempts == baseline.assembly_attempts);
    assert(buffered.payload_storage_reallocations -
               baseline.payload_storage_reallocations < 32);

    h.push(rtp(static_cast<uint16_t>(1001 + packet_count),
               2000,
               true,
               {0x61, 0x22}),
           2);
    const auto completed = h.jitter.stats();
    assert(completed.assembly_attempts == baseline.assembly_attempts + 1);
    assert(h.frames.size() == 2);
}

void test_nacks_are_rate_limited_per_window() {
    Harness h;
    openWithIdr(h, 2000, 1000);

    for (uint16_t sequence = 2002; sequence <= 2020; sequence += 2) {
        h.push(rtp(sequence, 1000, false, {}, 8), 1);
    }
    assert(h.nacks.size() == 8);

    h.push(rtp(2022, 1000, false, {}, 8), 60);
    assert(h.nacks.size() == 16);
    assert(h.nacks[8].first == 2017);
    assert(h.nacks[9].first == 2019);
    assert(h.nacks[10].first == 2021);
}

void test_nack_retries_while_retransmission_can_meet_deadline() {
    Harness h;
    h.jitter.setHoldMs(80);
    h.jitter.setNetworkRttMs(10);
    openWithIdr(h, 2050, 1000);

    h.push(rtp(2051, 2000, false, {0x7c, 0x81, 0x11}), 1);
    h.push(rtp(2053, 2000, true, {0x7c, 0x41, 0x33}), 2);
    assert(h.nacks.size() == 1);

    h.push(rtp(2054, 3000, false, {}, 8), 25);
    assert(h.nacks.size() == 2);
    assert(h.nacks.back().first == 2052);

    h.push(rtp(2055, 3000, false, {}, 8), 50);
    assert(h.nacks.size() == 3);
    assert(h.jitter.stats().nack_retries == 2);

    h.push(rtp(2056, 3000, false, {}, 8), 75);
    assert(h.nacks.size() == 3);
}

void test_nack_does_not_retry_past_high_rtt_frame_deadline() {
    Harness h;
    h.jitter.setHoldMs(180);
    h.jitter.setNetworkRttMs(130);
    openWithIdr(h, 2070, 1000);

    h.push(rtp(2071, 2000, false, {0x7c, 0x81, 0x11}), 1);
    h.push(rtp(2073, 2000, true, {0x7c, 0x41, 0x33}), 2);
    assert(h.nacks.size() == 1);

    h.push(rtp(2074, 3000, false, {}, 8), 70);
    assert(h.nacks.size() == 1);
    assert(h.jitter.stats().nack_retries == 0);
}

void test_log_sized_gap_is_fully_covered() {
    Harness h;
    openWithIdr(h, 2200, 1000);

    // lunarnx_drop_2.log recorded a 153-packet jump. Generic NACK covers
    // at most 17 sequence numbers, so complete coverage requires 9 reports.
    h.push(rtp(2354, 1000, false, {}, 8), 1);
    assert(h.nacks.size() == 8);
    h.push(rtp(2355, 1000, false, {}, 8), 60);
    assert(h.nacks.size() == 9);
}

void test_recovery_nacks_only_current_keyframe() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 2100, 1000);

    h.push(rtp(2101, 2000, false, {0x7c, 0x85, 0x11}), 1);
    h.push(rtp(2102, 3000, true, {0x61, 0x22}), 60);
    assert(h.jitter.waitingForKeyframe());

    const size_t nacks_before = h.nacks.size();
    h.push(rtp(2104, 4000, true, {0x61, 0x44}), 61);
    assert(h.nacks.size() == nacks_before);

    h.push(rtp(2105, 5000, false, {0x7c, 0x85, 0x55}), 62);
    h.push(rtp(2107, 5000, true, {0x7c, 0x45, 0x77}), 63);
    assert(h.nacks.size() == nacks_before + 1);
    assert(h.nacks.back().first == 2106);
    assert(h.jitter.waitingForKeyframe());

    h.push(rtp(2106, 5000, false, {0x7c, 0x05, 0x66}), 64);
    assert(h.frames.size() == 2);
    assert(!h.jitter.waitingForKeyframe());
}

void test_recovery_keyframe_nacks_remain_rate_limited() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 2400, 1000);

    h.push(rtp(2401, 2000, false, {0x7c, 0x85, 0x11}), 1);
    h.push(rtp(2402, 3000, true, {0x61, 0x22}), 60);
    assert(h.jitter.waitingForKeyframe());

    h.push(rtp(2403, 4000, false, {0x7c, 0x85, 0x33}), 61);
    h.push(rtp(2557, 4000, false, {0x7c, 0x05, 0x44}), 62);
    assert(h.nacks.size() == 8);

    h.push(rtp(2558, 4000, false, {0x7c, 0x05, 0x55}), 120);
    assert(h.nacks.size() == 9);
}

void test_recovery_discards_pending_nacks_from_old_stream() {
    Harness h;
    openWithIdr(h, 2600, 1000);

    h.push(rtp(2754, 1000, false, {}, 8), 1);
    assert(h.nacks.size() == 8);

    h.push(rtp(2755, 2000, true, {0x7c, 0x85, 0x11}), 2);
    assert(h.jitter.waitingForKeyframe());

    h.push(rtp(2757, 3000, true, {0x61, 0x22}), 60);
    assert(h.nacks.size() == 8);
}

void test_recovery_nacks_sps_pps_gap_after_idr_is_identified() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 3000, 1000);

    h.push(rtp(3001, 2000, false, {0x7c, 0x85, 0x11}), 1);
    h.push(rtp(3002, 3000, true, {0x61, 0x22}), 60);
    assert(h.jitter.waitingForKeyframe());

    const size_t nacks_before = h.nacks.size();
    h.push(rtp(3003, 4000, false, {0x67, 0x42}), 61);
    h.push(rtp(3005, 4000, false, {0x68, 0x11}), 62);
    assert(h.nacks.size() == nacks_before);

    h.push(rtp(3006, 4000, true, {0x65, 0x33}), 63);
    assert(h.nacks.size() == nacks_before + 1);
    assert(h.nacks.back().first == 3004);
    assert(h.jitter.waitingForKeyframe());

    h.push(rtp(3004, 4000, false, {0x68, 0x22}), 64);
    assert(h.frames.size() == 2);
    assert(!h.jitter.waitingForKeyframe());
}

void test_recovery_hold_does_not_increase_normal_frame_latency() {
    Harness normal;
    normal.jitter.setHoldMs(50);
    normal.jitter.setRecoveryHoldMs(300);
    openWithIdr(normal, 3200, 1000);
    normal.push(rtp(3201, 2000, false, {0x7c, 0x81, 0x11}), 1);
    normal.push(rtp(3202, 3000, true, {0x61, 0x22}), 60);
    assert(normal.jitter.stats().corrupt_frames == 1);
    assert(normal.frames.size() == 2);
    assert(!normal.jitter.waitingForKeyframe());

    Harness recovery;
    recovery.jitter.setHoldMs(50);
    recovery.jitter.setRecoveryHoldMs(300);
    openWithIdr(recovery, 3300, 1000);
    recovery.push(rtp(3301, 2000, true, {0x78, 0x00, 0x05, 0x67}), 1);
    assert(recovery.jitter.waitingForKeyframe());
    const auto corrupt_before = recovery.jitter.stats().corrupt_frames;

    recovery.push(rtp(3302, 3000, false, {0x67, 0x42}), 2);
    recovery.push(rtp(3303, 4000, false, {}, 8), 150);
    assert(recovery.jitter.stats().corrupt_frames == corrupt_before);
    assert(recovery.jitter.stats().buffered_frames == 1);

    recovery.push(rtp(3304, 4000, false, {}, 8), 303);
    assert(recovery.jitter.stats().corrupt_frames == corrupt_before + 1);
    assert(recovery.jitter.stats().buffered_frames == 0);
    assert(recovery.jitter.waitingForKeyframe());
}

void test_frame_backlog_bounds_head_of_line_wait() {
    Harness h;
    h.jitter.setHoldMs(180);
    h.jitter.setNetworkRttMs(60);
    openWithIdr(h, 3500, 1000);

    h.push(rtp(3501, 2000, false, {0x7c, 0x81, 0x11}), 1);
    h.push(rtp(3503, 2000, true, {0x7c, 0x41, 0x33}), 2);
    for (uint16_t index = 0; index < 9; ++index) {
        h.push(rtp(static_cast<uint16_t>(3504 + index),
                   3000 + static_cast<uint32_t>(index) * 1000,
                   true,
                   {0x61, static_cast<uint8_t>(index)}),
               20 + static_cast<uint64_t>(index) * 10);
    }

    assert(h.jitter.stats().corrupt_frames == 1);
    assert(h.jitter.stats().buffered_frames == 0);
    assert(h.frames.size() == 10);
    assert(!h.jitter.waitingForKeyframe());
}

} // namespace

int main() {
    test_reorders_retransmitted_fu_a();
    test_repeated_timeouts_gate_p_frames_until_real_idr();
    test_active_frame_progress_extends_idle_deadline();
    test_active_frame_progress_survives_old_hard_deadline();
    test_pathological_active_frame_obeys_safety_cap_without_decoder_reset();
    test_isolated_p_frame_timeout_uses_soft_recovery();
    test_incomplete_idr_waits_for_recovery_without_decoder_reset();
    test_padding_consumes_sequence_without_false_loss();
    test_late_packet_repairs_receiver_report_loss();
    test_missing_detection_totals_do_not_roll_back_after_recovery();
    test_packet_before_initial_sequence_is_not_reported_as_recovered();
    test_tracks_arrival_gap_sequence_jump_and_ssrc_change();
    test_timestamp_restart_resets_source_state();
    test_stale_retransmission_does_not_clear_recovery_state();
    test_missing_marker_does_not_block_later_idr();
    test_sequence_wrap_reorders_retransmission();
    test_malformed_payload_is_not_reported_as_overflow();
    test_access_unit_limit_is_enforced_without_unbounded_growth();
    test_buffer_caps_are_hard_limits();
    test_large_frame_defers_assembly_and_reuses_payload_storage();
    test_nacks_are_rate_limited_per_window();
    test_nack_retries_while_retransmission_can_meet_deadline();
    test_nack_does_not_retry_past_high_rtt_frame_deadline();
    test_log_sized_gap_is_fully_covered();
    test_recovery_nacks_only_current_keyframe();
    test_recovery_keyframe_nacks_remain_rate_limited();
    test_recovery_discards_pending_nacks_from_old_stream();
    test_recovery_nacks_sps_pps_gap_after_idr_is_identified();
    test_recovery_hold_does_not_increase_normal_frame_latency();
    test_frame_backlog_bounds_head_of_line_wait();
    std::cout << "Video RTP jitter buffer tests passed\n";
    return 0;
}
