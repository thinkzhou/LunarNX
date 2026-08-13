#include "gamepad_reader.h"
#include "../diagnostics.h"
#include <cstdio>
#include <cstring>
#include <new>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <SDL2/SDL.h>
#endif

namespace lunar::input {

GamepadReader::GamepadReader(ButtonMappingProfile profile) {
#ifdef __SWITCH__
    mapping_profile_ = profile;
    button_mapping_ = defaultButtonMapping(profile);
#else
    (void)profile;
#endif
}
GamepadReader::~GamepadReader() {
#ifdef __SWITCH__
    delete static_cast<PadState*>(pad_state_);
#endif
}

bool GamepadReader::initialize() {
#ifdef __SWITCH__
    reloadButtonMapping();
    initialized_ = false;
    delete static_cast<PadState*>(pad_state_);
    pad_state_ = nullptr;

    lunar::diagnosticLog("gamepad", "pad configure begin");
    PadState* pad = new (std::nothrow) PadState();
    if (!pad) {
        lunar::diagnosticLog("gamepad", "PadState allocation failed");
        return false;
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    lunar::diagnosticLog("gamepad", "pad initialize begin");
    padInitializeDefault(pad);
    pad_state_ = pad;
    lunar::diagnosticLog("gamepad", "pad initialize done");
#endif
    initialized_ = true;
    lunar::diagnosticLog("gamepad", "initialize done");
    return true;
}

void GamepadReader::reloadButtonMapping() {
#ifdef __SWITCH__
    button_mapping_ = loadButtonMapping(mapping_profile_);
#endif
}

GamepadState GamepadReader::read() {
    GamepadState state = {};

#ifdef __SWITCH__
    auto* pad = static_cast<PadState*>(pad_state_);
    padUpdate(pad);
    u64 btns = padGetButtons(pad);

    const bool quick_menu_chord =
        (btns & HidNpadButton_Minus) && (btns & HidNpadButton_Plus);
    if (quick_menu_chord) {
        btns &= ~(HidNpadButton_Minus | HidNpadButton_Plus);
    }

    uint64_t consumed = 0;
    for (uint64_t mapping : button_mapping_) {
        if (mapping != 0 && (mapping & (mapping - 1)) != 0 &&
            (btns & mapping) == mapping) {
            consumed |= mapping;
        }
    }
    auto mapped = [this, btns, consumed](RemoteButton button) {
        const uint64_t mapping = button_mapping_[static_cast<size_t>(button)];
        if (mapping == 0 || (btns & mapping) != mapping) return false;
        const bool combo = (mapping & (mapping - 1)) != 0;
        return combo || (consumed & mapping) == 0;
    };

    state.a = mapped(RemoteButton::A);
    state.b = mapped(RemoteButton::B);
    state.x = mapped(RemoteButton::X);
    state.y = mapped(RemoteButton::Y);
    state.dpad_up = mapped(RemoteButton::DpadUp);
    state.dpad_down = mapped(RemoteButton::DpadDown);
    state.dpad_left = mapped(RemoteButton::DpadLeft);
    state.dpad_right = mapped(RemoteButton::DpadRight);
    state.lb = mapped(RemoteButton::Lb);
    state.rb = mapped(RemoteButton::Rb);
    state.lt = mapped(RemoteButton::Lt);
    state.rt = mapped(RemoteButton::Rt);
    state.l3 = mapped(RemoteButton::L3);
    state.r3 = mapped(RemoteButton::R3);
    state.view = mapped(RemoteButton::View);
    state.menu = mapped(RemoteButton::Menu);
    state.guide = mapped(RemoteButton::Guide);
    state.touchpad = mapped(RemoteButton::Touchpad);

    HidAnalogStickState left = padGetStickPos(pad, 0);
    HidAnalogStickState right = padGetStickPos(pad, 1);

    auto apply_deadzone = [](s32 val) -> int16_t {
        if (val > -2000 && val < 2000) return 0;
        return static_cast<int16_t>(val);
    };

    state.left_stick_x  = apply_deadzone(left.x);
    state.left_stick_y  = apply_deadzone(left.y);
    state.right_stick_x = apply_deadzone(right.x);
    state.right_stick_y = apply_deadzone(right.y);

    state.left_trigger  = state.lt ? 65535 : 0;
    state.right_trigger = state.rt ? 65535 : 0;

#else
    // Desktop: optional SDL2 keyboard (for testing). Never throw if SDL is not ready.
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0 && SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        return state;
    }
    SDL_PumpEvents();
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (!keys) {
        return state;
    }

    state.a = keys[SDL_SCANCODE_RETURN];
    state.b = keys[SDL_SCANCODE_SPACE];
    state.x = keys[SDL_SCANCODE_X];
    state.y = keys[SDL_SCANCODE_Y];
    state.dpad_up    = keys[SDL_SCANCODE_UP];
    state.dpad_down  = keys[SDL_SCANCODE_DOWN];
    state.dpad_left  = keys[SDL_SCANCODE_LEFT];
    state.dpad_right = keys[SDL_SCANCODE_RIGHT];
    state.lb = keys[SDL_SCANCODE_LEFTBRACKET];
    state.rb = keys[SDL_SCANCODE_RIGHTBRACKET];
    state.lt = keys[SDL_SCANCODE_Q];
    state.rt = keys[SDL_SCANCODE_E];
    state.l3 = keys[SDL_SCANCODE_Z];
    state.r3 = keys[SDL_SCANCODE_C];
    state.view = keys[SDL_SCANCODE_TAB];
    state.menu = keys[SDL_SCANCODE_ESCAPE];

    float lx = (keys[SDL_SCANCODE_D] ? 1.0f : 0) - (keys[SDL_SCANCODE_A] ? 1.0f : 0);
    float ly = (keys[SDL_SCANCODE_S] ? 1.0f : 0) - (keys[SDL_SCANCODE_W] ? 1.0f : 0);
    state.left_stick_x  = static_cast<int16_t>(lx * 32767);
    state.left_stick_y  = static_cast<int16_t>(-ly * 32767);

    float rx = (keys[SDL_SCANCODE_L] ? 1.0f : 0) - (keys[SDL_SCANCODE_J] ? 1.0f : 0);
    float ry = (keys[SDL_SCANCODE_K] ? 1.0f : 0) - (keys[SDL_SCANCODE_I] ? 1.0f : 0);
    state.right_stick_x = static_cast<int16_t>(rx * 32767);
    state.right_stick_y = static_cast<int16_t>(-ry * 32767);

    state.left_trigger  = keys[SDL_SCANCODE_Q] ? 65535 : 0;
    state.right_trigger = keys[SDL_SCANCODE_E] ? 65535 : 0;
#endif
    return state;
}

bool GamepadReader::isConnected() const {
    return initialized_;
}

} // namespace lunar::input
