#include "stream/audio_packet_reorder.h"

#include <cassert>
#include <cstdint>
#include <vector>

using lunar::stream::AudioPacketReorder;
using lunar::stream::AudioReorderAction;
using lunar::stream::EncodedAudioPacket;

namespace {

EncodedAudioPacket packet(uint16_t sequence) {
    EncodedAudioPacket result;
    result.sequence = sequence;
    result.timestamp = static_cast<uint64_t>(sequence) * 20'000'000ULL;
    result.generation = 1;
    result.data = {static_cast<uint8_t>(sequence & 0xff)};
    return result;
}

void requirePacket(const AudioReorderAction& action, uint16_t sequence) {
    assert(action.type == AudioReorderAction::Type::Packet);
    assert(action.sequence == sequence);
    assert(action.packet.sequence == sequence);
}

void requireMissing(const AudioReorderAction& action, uint16_t sequence) {
    assert(action.type == AudioReorderAction::Type::Missing);
    assert(action.sequence == sequence);
}

} // namespace

int main() {
    AudioPacketReorder in_order;
    auto actions = in_order.push(packet(10));
    assert(actions.size() == 1);
    requirePacket(actions[0], 10);

    AudioPacketReorder reordered;
    requirePacket(reordered.push(packet(10))[0], 10);
    assert(reordered.push(packet(12)).empty());
    assert(reordered.push(packet(13)).empty());
    assert(reordered.push(packet(14)).empty());
    actions = reordered.push(packet(11));
    assert(actions.size() == 4);
    requirePacket(actions[0], 11);
    requirePacket(actions[1], 12);
    requirePacket(actions[2], 13);
    requirePacket(actions[3], 14);

    AudioPacketReorder missing;
    requirePacket(missing.push(packet(10))[0], 10);
    assert(missing.push(packet(12)).empty());
    assert(missing.push(packet(13)).empty());
    assert(missing.push(packet(14)).empty());
    actions = missing.push(packet(15));
    assert(actions.size() == 5);
    requireMissing(actions[0], 11);
    requirePacket(actions[1], 12);
    requirePacket(actions[2], 13);
    requirePacket(actions[3], 14);
    requirePacket(actions[4], 15);

    AudioPacketReorder late;
    requirePacket(late.push(packet(10))[0], 10);
    requirePacket(late.push(packet(11))[0], 11);
    assert(late.push(packet(10)).empty());

    AudioPacketReorder wrapping;
    requirePacket(wrapping.push(packet(65535))[0], 65535);
    actions = wrapping.push(packet(0));
    assert(actions.size() == 1);
    requirePacket(actions[0], 0);

    AudioPacketReorder discontinuity;
    requirePacket(discontinuity.push(packet(10))[0], 10);
    actions = discontinuity.push(packet(100));
    assert(actions.size() == 1);
    requirePacket(actions[0], 100);
    return 0;
}
