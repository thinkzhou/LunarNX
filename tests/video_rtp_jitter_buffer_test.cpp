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
            },
            [this](bool reset_decoder) {
                recovery_requests++;
                if (reset_decoder) decoder_resets++;
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
    assert(h.decoder_resets == 1);
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

void test_active_frame_still_obeys_hard_deadline() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 140, 1000);

    h.push(rtp(141, 2000, false, {0x7c, 0x81, 0x11}), 1);
    for (uint16_t sequence = 142; sequence <= 148; ++sequence) {
        h.push(rtp(sequence, 2000, false, {0x7c, 0x01, 0x22}),
               40 * (sequence - 140));
    }

    assert(h.jitter.stats().corrupt_frames == 1);
    assert(h.jitter.stats().buffered_frames == 0);
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

void test_incomplete_idr_uses_hard_recovery() {
    Harness h;
    h.jitter.setHoldMs(50);
    openWithIdr(h, 180, 1000);

    h.push(rtp(181, 2000, false, {0x7c, 0x85, 0x11}), 1);
    h.push(rtp(182, 3000, true, {0x61, 0x22}), 60);

    assert(h.jitter.stats().corrupt_frames == 1);
    assert(h.frames.size() == 1);
    assert(h.jitter.waitingForKeyframe());
    assert(h.recovery_requests == 1);
    assert(h.decoder_resets == 1);
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

    h.push(rtp(362, 3000, true, {0x65, 0x22}), 2);
    assert(!h.jitter.waitingForKeyframe());
    h.push(rtp(363, 4000, true, {0x7c, 0x81, 0x33}), 3);
    const auto fu_stats = h.jitter.stats();
    assert(fu_stats.corrupt_frames == 2);
    assert(fu_stats.overflow_frames == 0);
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

} // namespace

int main() {
    test_reorders_retransmitted_fu_a();
    test_repeated_timeouts_gate_p_frames_until_real_idr();
    test_active_frame_progress_extends_idle_deadline();
    test_active_frame_still_obeys_hard_deadline();
    test_isolated_p_frame_timeout_uses_soft_recovery();
    test_incomplete_idr_uses_hard_recovery();
    test_padding_consumes_sequence_without_false_loss();
    test_late_packet_repairs_receiver_report_loss();
    test_tracks_arrival_gap_sequence_jump_and_ssrc_change();
    test_stale_retransmission_does_not_clear_recovery_state();
    test_missing_marker_does_not_block_later_idr();
    test_sequence_wrap_reorders_retransmission();
    test_malformed_payload_is_not_reported_as_overflow();
    test_access_unit_limit_is_enforced_without_unbounded_growth();
    test_buffer_caps_are_hard_limits();
    test_large_frame_defers_assembly_and_reuses_payload_storage();
    std::cout << "Video RTP jitter buffer tests passed\n";
    return 0;
}
