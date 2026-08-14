#pragma once

#ifdef __SWITCH__

#include "ps_input_mapper.h"
#include <switch.h>
#include <array>
#include <chrono>
#include <cstdint>

namespace lunar::ps {

enum class PsTouchpadGesture : uint8_t {
    None,
    Touch,
    Tap,
    Pan,
    LongPress,
};

struct PsTouchpadFeedbackPoint {
    bool active = false;
    uint16_t screen_x = 0;
    uint16_t screen_y = 0;
};

struct PsTouchpadFeedback {
    PsTouchpadGesture gesture = PsTouchpadGesture::None;
    std::array<PsTouchpadFeedbackPoint, CHIAKI_CONTROLLER_TOUCHES_MAX> points{};
};

class PsTouchpadReader {
public:
    explicit PsTouchpadReader(bool ps5);

    PsTouchpadState read(bool suppressed);
    PsTouchpadFeedback feedback() const;
    void reset();

private:
    enum class GestureState : uint8_t {
        Idle,
        Pending,
        Pan,
        LongPress,
        ReleaseHold,
    };

    PsTouchpadState currentState(bool pressed) const;
    void updateTrackedTouches(const HidTouchScreenState& state);
    size_t activeTouchCount() const;

    struct TrackedTouch {
        bool active = false;
        uint32_t finger_id = 0;
        uint32_t down_x = 0;
        uint32_t down_y = 0;
        uint16_t screen_x = 0;
        uint16_t screen_y = 0;
        uint16_t x = 0;
        uint16_t y = 0;
    };

    bool ps5_ = false;
    bool blocked_until_release_ = false;
    GestureState gesture_ = GestureState::Idle;
    std::array<TrackedTouch, CHIAKI_CONTROLLER_TOUCHES_MAX> touches_{};
    uint32_t primary_finger_id_ = 0;
    uint32_t max_distance_squared_ = 0;
    bool had_multiple_touches_ = false;
    bool release_was_long_press_ = false;
    std::chrono::steady_clock::time_point gesture_started_{};
    std::chrono::steady_clock::time_point release_hold_until_{};
};

} // namespace lunar::ps

#endif
