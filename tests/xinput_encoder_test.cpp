#include "input/xinput_encoder.h"
#include "input/gamepad_reader.h"

#include <cassert>
#include <cstdint>
#include <vector>

using lunar::input::GamepadState;
using lunar::input::XInputEncoder;

namespace {

uint16_t readU16(const std::vector<uint8_t>& packet, size_t offset) {
    return static_cast<uint16_t>(packet[offset]) |
           (static_cast<uint16_t>(packet[offset + 1]) << 8);
}

int16_t readS16(const std::vector<uint8_t>& packet, size_t offset) {
    return static_cast<int16_t>(readU16(packet, offset));
}

uint32_t readU32(const std::vector<uint8_t>& packet, size_t offset) {
    return static_cast<uint32_t>(packet[offset]) |
           (static_cast<uint32_t>(packet[offset + 1]) << 8) |
           (static_cast<uint32_t>(packet[offset + 2]) << 16) |
           (static_cast<uint32_t>(packet[offset + 3]) << 24);
}

} // namespace

int main() {
    XInputEncoder encoder;
    GamepadState neutral;

    auto first = encoder.encode(neutral);
    auto repeated = encoder.encode(neutral);
    assert(first.size() == 38);
    assert(repeated.size() == 38);
    assert(readU16(first, 0) == 2);
    assert(readU32(first, 2) == 0);
    assert(readU32(repeated, 2) == 0);
    assert(XInputEncoder::stampSequence(first.data(), first.size(), 41));
    assert(XInputEncoder::stampSequence(repeated.data(), repeated.size(), 42));
    assert(readU32(first, 2) == 41);
    assert(readU32(repeated, 2) == 42);
    assert(readU32(first, 30) == 0);
    assert(readU32(first, 34) == 0);

    GamepadState a;
    a.a = true;
    auto a_packet = encoder.encode(a);
    assert(readU16(a_packet, 16) == 0x0010);
    assert(readU32(a_packet, 30) == 0x00001000);

    GamepadState guide;
    guide.guide = true;
    auto guide_packet = encoder.encode(guide);
    assert(readU16(guide_packet, 16) == 0x0002);
    assert(readU32(guide_packet, 30) == 0x00000400);

    GamepadState left_up;
    left_up.left_stick_y = 32767;
    auto stick_packet = encoder.encode(left_up);
    assert(readS16(stick_packet, 20) == 32767);
    assert(readU32(stick_packet, 30) == 0x000c0000);

    GamepadState trigger;
    trigger.left_trigger = 65535;
    auto trigger_packet = encoder.encode(trigger);
    assert(readU16(trigger_packet, 26) == 65535);
    assert(readU32(trigger_packet, 30) == 0x00010000);

    assert(encoder.encodeFrames({}).empty());

    GamepadState b;
    b.b = true;
    const auto two_frames = encoder.encodeFrames({a, b});
    assert(two_frames.size() == 61);
    assert(two_frames[14] == 2);
    assert(readU16(two_frames, 16) == 0x0010);
    assert(readU16(two_frames, 39) == 0x0020);

    std::vector<GamepadState> oversized(XInputEncoder::kMaxGamepadFrames + 1);
    const auto capped_frames = encoder.encodeFrames(oversized);
    assert(capped_frames.size() == 14 + 1 + XInputEncoder::kMaxGamepadFrames * 23);
    assert(capped_frames[14] == XInputEncoder::kMaxGamepadFrames);

    auto metadata = encoder.encodeMetadata(0);
    auto first_gamepad = encoder.encode(neutral);
    assert(XInputEncoder::stampSequence(metadata.data(), metadata.size(), 0));
    assert(XInputEncoder::stampSequence(
        first_gamepad.data(), first_gamepad.size(), 1));
    assert(readU32(metadata, 2) == 0);
    assert(readU32(first_gamepad, 2) == 1);

    assert(!XInputEncoder::stampSequence(nullptr, 6, 7));
    for (size_t len = 0; len < 6; ++len) {
        std::vector<uint8_t> sentinel(8, 0xa5);
        assert(!XInputEncoder::stampSequence(sentinel.data(), len, 7));
        for (uint8_t value : sentinel) assert(value == 0xa5);
    }
    return 0;
}
