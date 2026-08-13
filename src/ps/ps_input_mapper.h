#pragma once

#ifdef __SWITCH__

#include <chiaki/controller.h>
#include "../input/gamepad_reader.h"
#include <array>

namespace lunar::ps {

struct PsTouchpadPoint {
    bool active = false;
    uint16_t x = 0;
    uint16_t y = 0;
};

struct PsTouchpadState {
    bool pressed = false;
    std::array<PsTouchpadPoint, CHIAKI_CONTROLLER_TOUCHES_MAX> touches{};
};

class PsInputMapper {
public:
    PsInputMapper() = default;

    ChiakiControllerState map(const input::GamepadState& state,
                              const PsTouchpadState& touchpad = {});
    void reset();

private:
    ChiakiControllerState prev_{};
    std::array<int8_t, CHIAKI_CONTROLLER_TOUCHES_MAX> touch_ids_{{-1, -1}};
    uint8_t next_touch_id_ = 0;
};

} // namespace lunar::ps

#endif
