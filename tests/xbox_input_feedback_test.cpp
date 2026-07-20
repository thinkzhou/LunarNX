#include "webrtc/xbox_input_feedback.h"

#include <cassert>
#include <cstdint>

using lunar::webrtc::XboxVibrationCommand;
using lunar::webrtc::parseXboxVibrationPacket;

int main() {
    const uint8_t vibration[] = {
        0x80, 0x00,
        0x00, 0x00, 25, 50, 75, 100,
        0xfa, 0x00, 0x0a, 0x00, 0x02,
    };
    XboxVibrationCommand command;
    assert(parseXboxVibrationPacket(vibration, sizeof(vibration), command));
    assert(command.left_motor == 0.25f);
    assert(command.right_motor == 0.50f);
    assert(command.left_trigger == 0.75f);
    assert(command.right_trigger == 1.00f);
    assert(command.duration_ms == 250);
    assert(command.delay_ms == 10);
    assert(command.repeat == 2);

    const uint8_t metadata_and_vibration[] = {
        0x90, 0x00,
        0xd0, 0x02, 0x00, 0x00,
        0x00, 0x05, 0x00, 0x00,
        0x00, 0x00, 10, 20, 30, 40,
        0x14, 0x00, 0x00, 0x00, 0x00,
    };
    assert(parseXboxVibrationPacket(metadata_and_vibration,
                                    sizeof(metadata_and_vibration), command));
    assert(command.left_motor == 0.10f);
    assert(command.right_motor == 0.20f);
    assert(command.left_trigger == 0.30f);
    assert(command.right_trigger == 0.40f);
    assert(command.duration_ms == 20);

    const uint8_t unsupported_mixed[] = {0x82, 0x00, 0x00, 0x00};
    assert(!parseXboxVibrationPacket(unsupported_mixed,
                                     sizeof(unsupported_mixed), command));
    assert(!parseXboxVibrationPacket(vibration, sizeof(vibration) - 1, command));
    return 0;
}
