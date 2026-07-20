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
    assert(readU32(repeated, 2) == 1);
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

    GamepadState guide_chord;
    guide_chord.lb = true;
    guide_chord.rb = true;
    guide_chord.menu = true;
    lunar::input::applyGuideChord(guide_chord);
    assert(guide_chord.guide);
    assert(!guide_chord.lb);
    assert(!guide_chord.rb);
    assert(!guide_chord.menu);

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

    encoder.reset();
    auto metadata = encoder.encodeMetadata(0);
    auto after_reset = encoder.encode(neutral);
    assert(readU32(metadata, 2) == 0);
    assert(readU32(after_reset, 2) == 1);
    return 0;
}
