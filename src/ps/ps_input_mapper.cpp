#ifdef __SWITCH__

#include "ps_input_mapper.h"
#include <algorithm>
#include <limits>

namespace lunar::ps {

static inline uint8_t scaleTrigger(uint16_t val) {
    // 0..65535 -> 0..255
    return static_cast<uint8_t>((static_cast<uint32_t>(val) * 255) / 65535);
}

static inline int16_t invertYAxis(int16_t val) {
    // libnx uses positive Y for up; Chiaki follows SDL and uses negative Y.
    // Saturate the asymmetric int16_t minimum instead of overflowing it.
    return val == std::numeric_limits<int16_t>::min()
        ? std::numeric_limits<int16_t>::max()
        : static_cast<int16_t>(-val);
}

ChiakiControllerState PsInputMapper::map(const input::GamepadState& state) {
    ChiakiControllerState s{};
    chiaki_controller_state_set_idle(&s);

    // Physical button mapping (Switch layout -> PlayStation layout)
    if (state.a)      s.buttons |= CHIAKI_CONTROLLER_BUTTON_CROSS;
    if (state.b)      s.buttons |= CHIAKI_CONTROLLER_BUTTON_MOON;
    if (state.x)      s.buttons |= CHIAKI_CONTROLLER_BUTTON_BOX;
    if (state.y)      s.buttons |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;

    if (state.dpad_up)    s.buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
    if (state.dpad_down)  s.buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
    if (state.dpad_left)  s.buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
    if (state.dpad_right) s.buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;

    if (state.lb) s.buttons |= CHIAKI_CONTROLLER_BUTTON_L1;
    if (state.rb) s.buttons |= CHIAKI_CONTROLLER_BUTTON_R1;
    if (state.l3) s.buttons |= CHIAKI_CONTROLLER_BUTTON_L3;
    if (state.r3) s.buttons |= CHIAKI_CONTROLLER_BUTTON_R3;

    if (state.view)  s.buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;
    if (state.menu)  s.buttons |= CHIAKI_CONTROLLER_BUTTON_OPTIONS;
    if (state.guide) s.buttons |= CHIAKI_CONTROLLER_BUTTON_PS;

    // Analog sticks use the same range but opposite Y-axis conventions.
    s.left_x = state.left_stick_x;
    s.left_y = invertYAxis(state.left_stick_y);
    s.right_x = state.right_stick_x;
    s.right_y = invertYAxis(state.right_stick_y);

    // Analog triggers
    s.l2_state = scaleTrigger(state.left_trigger);
    s.r2_state = scaleTrigger(state.right_trigger);

    // Digital triggers as analog button flags
    if (state.lt) s.buttons |= CHIAKI_CONTROLLER_ANALOG_BUTTON_L2;
    if (state.rt) s.buttons |= CHIAKI_CONTROLLER_ANALOG_BUTTON_R2;

    prev_ = s;
    return s;
}

void PsInputMapper::reset() {
    prev_ = ChiakiControllerState{};
}

} // namespace lunar::ps

#endif
