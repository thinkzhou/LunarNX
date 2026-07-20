#pragma once

#include <cstddef>
#include <cstdint>

namespace lunar::webrtc {

struct XboxVibrationCommand {
    float left_motor = 0.0f;
    float right_motor = 0.0f;
    float left_trigger = 0.0f;
    float right_trigger = 0.0f;
    uint16_t duration_ms = 0;
    uint16_t delay_ms = 0;
    uint8_t repeat = 0;
};

bool parseXboxVibrationPacket(const uint8_t* data,
                              size_t len,
                              XboxVibrationCommand& command);

} // namespace lunar::webrtc
