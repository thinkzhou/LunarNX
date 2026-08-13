#pragma once

#include "../common.h"
#include "button_mapping.h"
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
    bool touchpad = false;                 // PlayStation touchpad click

    // Analog sticks (-32768 to 32767)
    int16_t left_stick_x = 0, left_stick_y = 0;
    int16_t right_stick_x = 0, right_stick_y = 0;

    // Analog triggers (0 to 65535, from Xbox)
    uint16_t left_trigger = 0;
    uint16_t right_trigger = 0;
};

class GamepadReader {
public:
    explicit GamepadReader(
        ButtonMappingProfile profile = ButtonMappingProfile::Xbox);
    ~GamepadReader();

    bool initialize();
    void releaseCaptureButton();
    void reloadButtonMapping();
    GamepadState read();
    bool isConnected() const;

private:
#ifdef __SWITCH__
    ButtonMappingProfile mapping_profile_ = ButtonMappingProfile::Xbox;
    ButtonMapping button_mapping_ =
        defaultButtonMapping(ButtonMappingProfile::Xbox);
    bool capture_button_acquired_ = false;
#endif
    bool initialized_ = false;
#ifdef __SWITCH__
    void* pad_state_ = nullptr;  // PadState*
#endif
};

} // namespace lunar::input
