#include "webrtc/peer_manager.h"

#include <mbedtls/ssl.h>

#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
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
    static uint32_t nextInputSequence(const PeerManager& peer) {
        return peer.next_input_sequence_;
    }
    static bool prepareInput(const PeerManager& peer,
                             const Command& command,
                             std::vector<uint8_t>& packet) {
        return peer.prepareSequencedInputPayload(command, packet);
    }
    static void commitInput(PeerManager& peer, int result) {
        peer.commitSequencedInputResult(result);
    }
    static bool waitingForKeyframe(const PeerManager& peer) {
        return peer.video_jitter_.waitingForKeyframe();
    }
    static void receiveVideo(PeerManager& peer,
                             const std::vector<uint8_t>& packet,
                             uint64_t now_ms) {
        peer.video_jitter_.setHoldMs(50);
        peer.video_jitter_.receive(
            packet.data(), packet.size(), now_ms,
            [](const uint8_t*, size_t, uint16_t, uint32_t) {},
            [&peer](uint16_t pid, uint16_t blp) {
                return peer.enqueueNack(pid, blp);
            },
            [](bool) {});
    }
    static Command push(PeerManager& peer, Type type) {
        std::lock_guard<std::mutex> lock(peer.outbound_mutex_);
        Command command;
        command.type = type;
        command.id = peer.next_outbound_command_id_++;
        peer.outbound_commands_.push_back(command);
        return command;
    }
};

} // namespace lunar::webrtc

namespace {

std::vector<uint8_t> rtp(uint16_t sequence,
                         uint32_t timestamp,
                         bool marker,
                         const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet(12 + payload.size(), 0);
    packet[0] = 0x80;
    packet[1] = static_cast<uint8_t>(102 | (marker ? 0x80 : 0));
    packet[2] = static_cast<uint8_t>(sequence >> 8);
    packet[3] = static_cast<uint8_t>(sequence);
    packet[4] = static_cast<uint8_t>(timestamp >> 24);
    packet[5] = static_cast<uint8_t>(timestamp >> 16);
    packet[6] = static_cast<uint8_t>(timestamp >> 8);
    packet[7] = static_cast<uint8_t>(timestamp);
    packet[11] = 1;
    std::memcpy(packet.data() + 12, payload.data(), payload.size());
    return packet;
}

uint32_t readU32(const std::vector<uint8_t>& packet, size_t offset) {
    return static_cast<uint32_t>(packet[offset]) |
           (static_cast<uint32_t>(packet[offset + 1]) << 8) |
           (static_cast<uint32_t>(packet[offset + 2]) << 16) |
           (static_cast<uint32_t>(packet[offset + 3]) << 24);
}

} // namespace

int main() {
    using lunar::webrtc::PeerManager;
    using Access = lunar::webrtc::PeerManagerQueueTestAccess;

    PeerManager peer;
    Access::connect(peer);
    const uint8_t payload[] = {1, 2, 3};

    assert(peer.sendControlData(payload, sizeof(payload)));
    auto reliable = Access::front(peer);
    assert(Access::complete(peer, reliable, MBEDTLS_ERR_SSL_WANT_WRITE));
    assert(Access::size(peer) == 1);
    assert(!peer.consumeReliableSendFailure());
    assert(!Access::complete(peer, reliable, static_cast<int>(sizeof(payload))));
    assert(Access::size(peer) == 0);

    assert(peer.sendControlData(payload, sizeof(payload)));
    reliable = Access::front(peer);
    for (int attempt = 0; attempt < 8; ++attempt) {
        Access::complete(peer, reliable, MBEDTLS_ERR_SSL_WANT_WRITE);
        if (attempt < 7) {
            reliable = Access::front(peer);
        }
    }
    assert(Access::size(peer) == 0);
    assert(peer.consumeReliableSendFailure());

    const uint8_t input_draft_a[] = {2, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t input_draft_b[] = {2, 0, 0, 0, 0, 0, 1, 0};
    PeerManager sequenced;
    Access::connect(sequenced);
    assert(sequenced.sendLatestInputData(input_draft_a, sizeof(input_draft_a)));
    assert(sequenced.sendLatestInputData(input_draft_b, sizeof(input_draft_b)));
    assert(Access::nextInputSequence(sequenced) == 0);
    auto sequenced_command = Access::front(sequenced);
    std::vector<uint8_t> wire_packet;
    assert(Access::prepareInput(sequenced, sequenced_command, wire_packet));
    assert(readU32(wire_packet, 2) == 0);
    Access::commitInput(sequenced, MBEDTLS_ERR_SSL_WANT_WRITE);
    assert(Access::nextInputSequence(sequenced) == 0);
    assert(!Access::complete(
        sequenced, sequenced_command, MBEDTLS_ERR_SSL_WANT_WRITE));

    assert(sequenced.sendLatestInputData(input_draft_a, sizeof(input_draft_a)));
    sequenced_command = Access::front(sequenced);
    assert(Access::prepareInput(sequenced, sequenced_command, wire_packet));
    assert(readU32(wire_packet, 2) == 0);
    Access::commitInput(sequenced, static_cast<int>(wire_packet.size()));
    assert(Access::nextInputSequence(sequenced) == 1);
    assert(!Access::complete(
        sequenced, sequenced_command, static_cast<int>(wire_packet.size())));

    assert(sequenced.sendInputData(input_draft_a, sizeof(input_draft_a)));
    sequenced_command = Access::front(sequenced);
    assert(Access::prepareInput(sequenced, sequenced_command, wire_packet));
    assert(readU32(wire_packet, 2) == 1);
    Access::commitInput(sequenced, MBEDTLS_ERR_SSL_WANT_WRITE);
    assert(Access::nextInputSequence(sequenced) == 1);
    assert(Access::complete(
        sequenced, sequenced_command, MBEDTLS_ERR_SSL_WANT_WRITE));
    sequenced_command = Access::front(sequenced);
    assert(Access::prepareInput(sequenced, sequenced_command, wire_packet));
    assert(readU32(wire_packet, 2) == 1);
    Access::commitInput(sequenced, static_cast<int>(wire_packet.size()));
    assert(Access::nextInputSequence(sequenced) == 2);
    assert(!Access::complete(
        sequenced, sequenced_command, static_cast<int>(wire_packet.size())));

    assert(peer.sendMessageData(payload, sizeof(payload)));
    reliable = Access::front(peer);
    assert(!Access::complete(peer, reliable, -1));
    assert(Access::size(peer) == 0);
    assert(peer.consumeReliableSendFailure());

    const uint8_t first_input[] = {4};
    const uint8_t latest_input[] = {5, 6};
    assert(peer.sendLatestInputData(first_input, sizeof(first_input)));
    const auto old_input = Access::front(peer);
    assert(peer.sendLatestInputData(latest_input, sizeof(latest_input)));
    const auto new_input = Access::front(peer);
    assert(Access::size(peer) == 1);
    assert(new_input.id != old_input.id);
    assert(new_input.payload == std::vector<uint8_t>(latest_input,
                                                      latest_input + 2));
    assert(!Access::complete(peer, new_input, MBEDTLS_ERR_SSL_WANT_WRITE));
    assert(Access::size(peer) == 0);

    assert(peer.requestVideoKeyframe());
    auto pli = Access::front(peer);
    assert(Access::complete(peer, pli, -1));
    assert(Access::size(peer) == 1);
    pli = Access::front(peer);
    assert(pli.attempts == 1);
    assert(Access::complete(peer, pli, -1));
    assert(Access::size(peer) == 1);
    pli = Access::front(peer);
    assert(pli.attempts == 2);
    assert(!Access::complete(peer, pli, -1));
    assert(Access::size(peer) == 0);

    auto nack = Access::push(peer, Access::Type::Nack);
    assert(Access::complete(peer, nack, -1));
    assert(Access::size(peer) == 1);
    nack = Access::front(peer);
    assert(nack.attempts == 1);
    assert(!Access::complete(peer, nack, -1));
    assert(Access::size(peer) == 0);

    assert(peer.sendControlData(payload, sizeof(payload)));
    assert(peer.sendLatestInputData(payload, sizeof(payload)));
    assert(peer.requestVideoKeyframe());
    const auto ordered = Access::types(peer);
    assert(ordered.size() == 3);
    assert(ordered[0] == Access::Type::Pli);
    assert(ordered[1] == Access::Type::InputLatest);
    assert(ordered[2] == Access::Type::Control);

    pli = Access::front(peer);
    assert(!Access::complete(peer, pli, 12));
    assert(Access::front(peer).type == Access::Type::InputLatest);

    PeerManager scheduler;
    Access::connect(scheduler);
    assert(scheduler.sendControlData(payload, sizeof(payload)));
    reliable = Access::front(scheduler);
    assert(Access::complete(scheduler, reliable, MBEDTLS_ERR_SSL_WANT_WRITE));
    assert(Access::enqueueNack(scheduler, 77, 0x0003));
    Access::Command selected;
    assert(Access::select(scheduler, selected, false));
    assert(selected.type == Access::Type::Nack);

    PeerManager recovery_nack;
    Access::connect(recovery_nack);
    Access::receiveVideo(recovery_nack,
                         rtp(100, 1000, true, {0x65, 0x11}), 0);
    Access::receiveVideo(recovery_nack,
                         rtp(101, 2000, false, {0x7c, 0x85, 0x22}), 1);
    Access::receiveVideo(recovery_nack,
                         rtp(102, 3000, true, {0x61, 0x33}), 60);
    assert(Access::waitingForKeyframe(recovery_nack));
    Access::receiveVideo(recovery_nack,
                         rtp(103, 4000, false, {0x67, 0x42}), 61);
    Access::receiveVideo(recovery_nack,
                         rtp(105, 4000, false, {0x68, 0x44}), 62);
    assert(Access::size(recovery_nack) == 0);
    Access::receiveVideo(recovery_nack,
                         rtp(106, 4000, true, {0x65, 0x55}), 63);
    assert(Access::size(recovery_nack) == 1);
    assert(Access::front(recovery_nack).type == Access::Type::Nack);
    assert(Access::front(recovery_nack).pid == 104);

    std::vector<uint8_t> oversized(1025, 0);
    assert(!peer.sendMessageData(oversized.data(), oversized.size()));

    PeerManager concurrent;
    Access::connect(concurrent);
    std::atomic<bool> producer_done{false};
    std::thread producer([&]() {
        for (uint32_t value = 0; value < 10000; ++value) {
            const uint8_t bytes[] = {
                2, 0, 0, 0,
                static_cast<uint8_t>(value),
                static_cast<uint8_t>(value >> 8),
                static_cast<uint8_t>(value >> 16),
                static_cast<uint8_t>(value >> 24),
            };
            assert(concurrent.sendLatestInputData(bytes, sizeof(bytes)));
        }
        producer_done = true;
    });
    while (!producer_done.load() || Access::size(concurrent) > 0) {
        if (Access::size(concurrent) > 0) {
            const auto snapshot = Access::front(concurrent);
            std::vector<uint8_t> packet;
            assert(Access::prepareInput(concurrent, snapshot, packet));
            assert(readU32(packet, 2) ==
                   Access::nextInputSequence(concurrent));
            Access::commitInput(concurrent, static_cast<int>(packet.size()));
            Access::complete(concurrent, snapshot,
                             static_cast<int>(snapshot.payload.size()));
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    assert(Access::size(concurrent) == 0);
    assert(Access::nextInputSequence(concurrent) > 0);

    return 0;
}
