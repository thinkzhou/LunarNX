#pragma once

#include "../common.h"
#include "gamepad_reader.h"
#include <vector>

namespace lunar::input {

class XInputEncoder {
public:
    XInputEncoder();
    ~XInputEncoder();

    void reset();
    std::vector<uint8_t> encodeMetadata(uint8_t max_touch_points = 0);
    std::vector<uint8_t> encode(const GamepadState& state);

private:
    uint32_t sequence_ = 0;
};

} // namespace lunar::input
