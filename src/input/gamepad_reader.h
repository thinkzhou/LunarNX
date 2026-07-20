#pragma once

#include "../common.h"
#include <cstdint>

namespace lunar::input {

// Xbox-aligned gamepad state (matches Xbox controller layout)
struct GamepadState {
    // Digital buttons (Xbox names)
    bool a = false, b = false, x = false, y = false;
    bool dpad_up = false, dpad_down = false, dpad_left = false, dpad_right = false;
    bool lb = false, rb = false;           // left/right bumper
    bool lt = false, rt = false;           // triggers (digital)
    bool l3 = false, r3 = false;           // stick clicks
    bool view = false, menu = false;       // back/start
    bool guide = false;                    // Xbox button

    // Analog sticks (-32768 to 32767)
    int16_t left_stick_x = 0, left_stick_y = 0;
    int16_t right_stick_x = 0, right_stick_y = 0;

    // Analog triggers (0 to 65535, from Xbox)
    uint16_t left_trigger = 0;
    uint16_t right_trigger = 0;
};

// L + R + Plus is reserved as the Xbox Guide chord while streaming. Suppress
// the component buttons so the console receives one unambiguous Nexus press.
inline void applyGuideChord(GamepadState& state) {
    if (!state.lb || !state.rb || !state.menu) return;

    state.guide = true;
    state.lb = false;
    state.rb = false;
    state.menu = false;
}

class GamepadReader {
public:
    GamepadReader();
    ~GamepadReader();

    bool initialize();
    GamepadState read();
    bool isConnected() const;

private:
    bool initialized_ = false;
#ifdef __SWITCH__
    void* pad_state_ = nullptr;  // PadState*
#endif
};

} // namespace lunar::input
