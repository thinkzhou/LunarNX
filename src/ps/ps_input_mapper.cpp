#ifdef __SWITCH__

#include "ps_input_mapper.h"
#include "ps_motion_reader.h"
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

ChiakiControllerState PsInputMapper::map(const input::GamepadState& state,
                                         const PsTouchpadState& touchpad,
                                         const PsMotionState* motion) {
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

    for (size_t i = 0; i < touchpad.touches.size(); ++i) {
        const auto& touch = touchpad.touches[i];
        if (touch.active) {
            if (touch_ids_[i] < 0) {
                touch_ids_[i] = static_cast<int8_t>(next_touch_id_);
                next_touch_id_ = static_cast<uint8_t>((next_touch_id_ + 1) & 0x7f);
            }
            s.touches[i].id = touch_ids_[i];
            s.touches[i].x = touch.x;
            s.touches[i].y = touch.y;
        } else {
            touch_ids_[i] = -1;
        }
    }
    s.touch_id_next = next_touch_id_;
    if (touchpad.pressed || state.touchpad) {
        s.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
    }

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

    if (motion && motion->valid) {
        s.gyro_x = motion->gyro_x;
        s.gyro_y = motion->gyro_y;
        s.gyro_z = motion->gyro_z;
        s.accel_x = motion->accel_x;
        s.accel_y = motion->accel_y;
        s.accel_z = motion->accel_z;
        s.orient_x = motion->orient_x;
        s.orient_y = motion->orient_y;
        s.orient_z = motion->orient_z;
        s.orient_w = motion->orient_w;
    }

    prev_ = s;
    return s;
}

void PsInputMapper::reset() {
    prev_ = ChiakiControllerState{};
    touch_ids_.fill(-1);
    next_touch_id_ = 0;
}

} // namespace lunar::ps

#endif
