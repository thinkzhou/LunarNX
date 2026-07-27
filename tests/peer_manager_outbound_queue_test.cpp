#include "webrtc/peer_manager.h"

#include <mbedtls/ssl.h>

#include <cassert>
#include <atomic>
#include <cstdint>
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
    assert(ordered[1] == Access::Type::Control);
    assert(ordered[2] == Access::Type::InputLatest);

    pli = Access::front(peer);
    assert(!Access::complete(peer, pli, 12));
    assert(Access::front(peer).type == Access::Type::Control);

    std::vector<uint8_t> oversized(1025, 0);
    assert(!peer.sendMessageData(oversized.data(), oversized.size()));

    PeerManager concurrent;
    Access::connect(concurrent);
    std::atomic<bool> producer_done{false};
    std::thread producer([&]() {
        for (uint32_t value = 0; value < 10000; ++value) {
            const uint8_t bytes[] = {
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
            Access::complete(concurrent, snapshot,
                             static_cast<int>(snapshot.payload.size()));
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    assert(Access::size(concurrent) == 0);

    return 0;
}
