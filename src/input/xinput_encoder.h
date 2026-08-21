#pragma once

#include "../common.h"
#include "gamepad_reader.h"
#include <cstddef>
#include <vector>

namespace lunar::input {

class XInputEncoder {
public:
    static constexpr size_t kMaxGamepadFrames = 29;

    XInputEncoder();
    ~XInputEncoder();

    std::vector<uint8_t> encodeMetadata(uint8_t max_touch_points = 0) const;
    std::vector<uint8_t> encode(const GamepadState& state) const;
    std::vector<uint8_t> encodeFrames(
        const std::vector<GamepadState>& states) const;
    static bool stampSequence(uint8_t* data, size_t len, uint32_t sequence);
};

} // namespace lunar::input
