#include "webrtc/peer_manager.h"
#include "webrtc/video_rtp_jitter_buffer.h"

#include <mbedtls/ssl.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <vector>

namespace lunar::webrtc {

struct PeerManagerQueueTestAccess {
    using Command = PeerManager::OutboundCommand;
    using Type = PeerManager::OutboundType;

    static void connect(PeerManager& peer) { peer.connected_ = true; }
    static size_t size(const PeerManager& peer) {
        std::lock_guard<std::mutex> lock(peer.outbound_mutex_);
        return peer.outbound_commands_.size();
    }
    static Command front(const PeerManager& peer) {
        std::lock_guard<std::mutex> lock(peer.outbound_mutex_);
        assert(!peer.outbound_commands_.empty());
        return peer.outbound_commands_.front();
    }
    static std::vector<Type> types(const PeerManager& peer) {
        std::lock_guard<std::mutex> lock(peer.outbound_mutex_);
        std::vector<Type> result;
        for (const auto& command : peer.outbound_commands_) {
            result.push_back(command.type);
        }
        return result;
    }
    static bool complete(PeerManager& peer, const Command& command, int result) {
        return peer.completeOutboundCommand(command, result);
    }
    static bool select(const PeerManager& peer,
                       Command& command,
                       bool allow_sctp) {
        return peer.selectOutboundCommand(command, allow_sctp);
    }
    static bool enqueueNack(PeerManager& peer, uint16_t pid, uint16_t blp) {
        return peer.enqueueNack(pid, blp);
    }
};

} // namespace lunar::webrtc

namespace {

using lunar::webrtc::VideoRtpJitterBuffer;

std::vector<uint8_t> rtp(uint16_t sequence,
                         uint32_t timestamp,
                         bool marker,
                         const std::vector<uint8_t>& payload,
                         size_t padding = 0) {
    std::vector<uint8_t> packet(12 + payload.size() + padding, 0);
    packet[0] = static_cast<uint8_t>(0x80 | (padding ? 0x20 : 0));
    packet[1] = static_cast<uint8_t>(102 | (marker ? 0x80 : 0));
    packet[2] = static_cast<uint8_t>(sequence >> 8);
    packet[3] = static_cast<uint8_t>(sequence);
    packet[4] = static_cast<uint8_t>(timestamp >> 24);
    packet[5] = static_cast<uint8_t>(timestamp >> 16);
    packet[6] = static_cast<uint8_t>(timestamp >> 8);
    packet[7] = static_cast<uint8_t>(timestamp);
    packet[11] = 1;
    std::memcpy(packet.data() + 12, payload.data(), payload.size());
    if (padding) packet.back() = static_cast<uint8_t>(padding);
    return packet;
}

struct JitterHarness {
    VideoRtpJitterBuffer jitter;
    size_t frames = 0;
    size_t recovery_requests = 0;
    size_t decoder_resets = 0;
    std::vector<std::pair<uint16_t, uint16_t>> nacks;

    void push(const std::vector<uint8_t>& packet, uint64_t now_ms) {
        jitter.receive(
            packet.data(), packet.size(), now_ms,
            [this](const uint8_t*, size_t, uint16_t, uint32_t) { frames++; },
            [this](uint16_t pid, uint16_t blp) {
                nacks.emplace_back(pid, blp);
                return true;
            },
            [this](bool reset_decoder) {
                recovery_requests++;
                if (reset_decoder) decoder_resets++;
            });
    }

    void open(uint16_t sequence) {
        push(rtp(sequence, 1000, true, {0x65, 0x11}), 0);
        assert(frames == 1);
        assert(!jitter.waitingForKeyframe());
    }
};

size_t nackCoverage(const std::vector<std::pair<uint16_t, uint16_t>>& nacks) {
    std::set<uint16_t> requested;
    for (const auto& [pid, blp] : nacks) {
        requested.insert(pid);
        for (unsigned bit = 0; bit < 16; ++bit) {
            if (blp & (1u << bit)) {
                requested.insert(static_cast<uint16_t>(pid + bit + 1));
            }
        }
    }
    return requested.size();
}

void scenarioSinglePacketRepair() {
    JitterHarness h;
    h.open(10);
    h.push(rtp(11, 2000, false, {0x7c, 0x81, 0x11}), 1);
    h.push(rtp(13, 2000, true, {0x7c, 0x41, 0x33}), 2);
    h.push(rtp(12, 2000, false, {0x7c, 0x01, 0x22}), 30);
    const auto stats = h.jitter.stats();
    assert(h.frames == 2);
    assert(h.nacks.size() == 1);
    assert(stats.corrupt_frames == 0);
    assert(h.recovery_requests == 0);
    std::cout << "METRIC scenario=single_packet_repair repair_ms=28 nacks=1 "
                 "corrupt_frames=0 hard_recovery=0\n";
}

void scenarioRecordedGaps() {
    JitterHarness gap52;
    gap52.open(100);
    gap52.push(rtp(153, 1000, false, {}, 8), 1);
    assert(gap52.nacks.size() == 4);
    assert(nackCoverage(gap52.nacks) == 52);
    std::cout << "METRIC scenario=recorded_gap_52 first_window_ms=0 nacks=4 "
                 "covered_packets=52\n";

    JitterHarness gap153;
    gap153.open(1000);
    gap153.push(rtp(1154, 1000, false, {}, 8), 1);
    const size_t first_window = gap153.nacks.size();
    gap153.push(rtp(1155, 1000, false, {}, 8), 60);
    assert(first_window == 8);
    assert(gap153.nacks.size() == 9);
    assert(nackCoverage(gap153.nacks) == 153);
    std::cout << "METRIC scenario=recorded_gap_153 first_window_nacks="
              << first_window
              << " total_nacks=9 covered_packets=153 coverage_ms=59\n";
}

void scenarioRecoveryRepairsCurrentKeyframe() {
    JitterHarness h;
    h.jitter.setHoldMs(50);
    h.open(2000);
    h.push(rtp(2001, 2000, false, {0x7c, 0x85, 0x11}), 1);
    h.push(rtp(2002, 3000, true, {0x61, 0x22}), 60);
    assert(h.jitter.waitingForKeyframe());
    const size_t before = h.nacks.size();
    h.push(rtp(2004, 4000, true, {0x61, 0x44}), 61);
    assert(h.nacks.size() == before);

    h.push(rtp(2005, 5000, false, {0x7c, 0x85, 0x55}), 62);
    h.push(rtp(2007, 5000, true, {0x7c, 0x45, 0x77}), 63);
    assert(h.nacks.size() == before + 1);
    assert(h.nacks.back().first == 2006);

    h.push(rtp(2006, 5000, false, {0x7c, 0x05, 0x66}), 64);
    assert(h.frames == 2);
    assert(!h.jitter.waitingForKeyframe());
    std::cout << "METRIC scenario=recovery_keyframe_repair old_frame_nacks=0 "
                 "keyframe_nacks=1 repaired=1 waiting=0\n";
}

void scenarioHighRttRecoveryPreservesCandidateIdr() {
    JitterHarness h;
    h.jitter.setHoldMs(180);
    h.jitter.setRecoveryHoldMs(720);
    h.open(3000);
    h.push(rtp(3001, 2000, true, {0x78, 0x00, 0x05, 0x67}), 1);
    assert(h.jitter.waitingForKeyframe());
    const auto corrupt_before = h.jitter.stats().corrupt_frames;

    h.push(rtp(3002, 3000, false, {0x67, 0x42}), 2);
    h.push(rtp(3004, 3000, true, {0x65, 0x44}), 3);
    assert(h.nacks.back().first == 3003);
    h.push(rtp(3005, 4000, false, {}, 8), 200);
    h.push(rtp(3006, 4000, false, {}, 8), 400);
    assert(h.jitter.stats().corrupt_frames == corrupt_before);

    h.push(rtp(3003, 3000, false, {0x68, 0x22}), 572);
    assert(h.frames == 2);
    assert(!h.jitter.waitingForKeyframe());
    assert(h.jitter.stats().corrupt_frames == corrupt_before);
    std::cout << "METRIC scenario=high_rtt_recovery rtt_ms=570 "
                 "recovery_hold_ms=720 repaired_after_ms=570 "
                 "additional_corrupt_frames=0 waiting=0\n";
}

void scenarioOutboundPriority() {
    using Access = lunar::webrtc::PeerManagerQueueTestAccess;
    lunar::webrtc::PeerManager peer;
    Access::connect(peer);
    const uint8_t first[] = {1};
    const uint8_t latest[] = {2};
    assert(peer.sendControlData(first, sizeof(first)));
    assert(peer.sendLatestInputData(first, sizeof(first)));
    assert(peer.requestVideoKeyframe());
    auto types = Access::types(peer);
    assert(types.front() == Access::Type::Pli);
    assert(!Access::complete(peer, Access::front(peer), 12));
    assert(Access::complete(peer, Access::front(peer),
                            MBEDTLS_ERR_SSL_WANT_WRITE));
    assert(Access::enqueueNack(peer, 77, 0x0003));
    Access::Command selected;
    assert(Access::select(peer, selected, false));
    assert(selected.type == Access::Type::Nack);
    assert(!Access::complete(peer, selected, 12));
    assert(peer.sendLatestInputData(latest, sizeof(latest)));
    types = Access::types(peer);
    assert(types.size() == 2);
    assert(types[0] == Access::Type::Control);
    assert(types[1] == Access::Type::InputLatest);
    std::cout << "METRIC scenario=control_backpressure pli_position=0 "
                 "nack_bypassed_sctp=1 pending_input_states=1 queue_size=2\n";
}

} // namespace

int main() {
    scenarioSinglePacketRepair();
    scenarioRecordedGaps();
    scenarioRecoveryRepairsCurrentKeyframe();
    scenarioHighRttRecoveryPreservesCandidateIdr();
    scenarioOutboundPriority();
    return 0;
}
