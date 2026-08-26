#ifdef __SWITCH__

#include "ps_touchpad_reader.h"
#include <switch.h>
#include <algorithm>

namespace lunar::ps {
namespace {

constexpr uint32_t kScreenMaxX = 1279;
constexpr uint32_t kScreenMaxY = 719;
constexpr uint32_t kTapMaxDistance = 10;
constexpr uint32_t kPanMinDistance = 30;
constexpr auto kTapMaxDuration = std::chrono::milliseconds(200);
constexpr auto kLongPressDelay = std::chrono::milliseconds(500);
constexpr auto kReleaseHoldDuration = std::chrono::milliseconds(150);

uint16_t scaleCoordinate(uint32_t value, uint32_t source_max, uint32_t target_max) {
    value = std::min(value, source_max);
    return static_cast<uint16_t>((value * target_max + source_max / 2) / source_max);
}

} // namespace

PsTouchpadReader::PsTouchpadReader(bool ps5) : ps5_(ps5) {
    // Borealis initializes HID during app startup. Enabling the touchscreen
    // here keeps PS-specific input lazy and scoped to an active PS stream.
    hidInitializeTouchScreen();
}

PsTouchpadState PsTouchpadReader::currentState(bool pressed) const {
    PsTouchpadState state{};
    state.pressed = pressed;
    for (size_t i = 0; i < touches_.size(); ++i) {
        state.touches[i] = {touches_[i].active, touches_[i].x, touches_[i].y};
    }
    return state;
}

size_t PsTouchpadReader::activeTouchCount() const {
    return std::count_if(touches_.begin(), touches_.end(),
                         [](const TrackedTouch& touch) { return touch.active; });
}

void PsTouchpadReader::updateTrackedTouches(const HidTouchScreenState& state) {
    std::array<bool, CHIAKI_CONTROLLER_TOUCHES_MAX> found{};
    const uint32_t max_x = ps5_ ? 1919 : 1920;
    const uint32_t max_y = ps5_ ? 1079 : 942;

    for (size_t input_index = 0; input_index < static_cast<size_t>(state.count);
         ++input_index) {
        const auto& input = state.touches[input_index];
        size_t slot = touches_.size();
        for (size_t i = 0; i < touches_.size(); ++i) {
            if (touches_[i].active && touches_[i].finger_id == input.finger_id) {
                slot = i;
                break;
            }
        }
        if (slot == touches_.size()) {
            for (size_t i = 0; i < touches_.size(); ++i) {
                if (!touches_[i].active) {
                    slot = i;
                    touches_[i].active = true;
                    touches_[i].finger_id = input.finger_id;
                    touches_[i].down_x = input.x;
                    touches_[i].down_y = input.y;
                    break;
                }
            }
        }
        if (slot == touches_.size()) continue;

        found[slot] = true;
        touches_[slot].screen_x = static_cast<uint16_t>(
            std::min(input.x, kScreenMaxX));
        touches_[slot].screen_y = static_cast<uint16_t>(
            std::min(input.y, kScreenMaxY));
        touches_[slot].x = scaleCoordinate(input.x, kScreenMaxX, max_x);
        touches_[slot].y = scaleCoordinate(input.y, kScreenMaxY, max_y);
    }

    for (size_t i = 0; i < touches_.size(); ++i) {
        if (touches_[i].active && !found[i]) touches_[i].active = false;
    }
}

PsTouchpadFeedback PsTouchpadReader::feedback() const {
    PsTouchpadFeedback feedback{};
    switch (gesture_) {
        case GestureState::Pending:
            feedback.gesture = PsTouchpadGesture::Touch;
            break;
        case GestureState::Pan:
            feedback.gesture = PsTouchpadGesture::Pan;
            break;
        case GestureState::LongPress:
            feedback.gesture = PsTouchpadGesture::LongPress;
            break;
        case GestureState::ReleaseHold:
            feedback.gesture = release_was_long_press_
                ? PsTouchpadGesture::LongPress
                : PsTouchpadGesture::Tap;
            break;
        case GestureState::Idle:
            return feedback;
    }
    for (size_t i = 0; i < touches_.size(); ++i) {
        feedback.points[i] = {
            touches_[i].active,
            touches_[i].screen_x,
            touches_[i].screen_y,
        };
    }
    return feedback;
}

PsTouchpadState PsTouchpadReader::read(bool suppressed) {
    const auto now = std::chrono::steady_clock::now();
    HidTouchScreenState state{};
    const size_t samples = hidGetTouchScreenStates(&state, 1);

    if (suppressed) {
        // Only fence off a touch that was already in progress when the UI
        // took ownership.  Setting this unconditionally makes a quiet UI
        // period depend on a later empty HID sample; if libnx has no new
        // sample queued, the first touch after returning to the game is
        // discarded until the user taps again.
        const bool had_active_touch = activeTouchCount() > 0 ||
            gesture_ != GestureState::Idle;
        if (had_active_touch) blocked_finger_id_ = primary_finger_id_;
        gesture_ = GestureState::Idle;
        touches_ = {};
        had_multiple_touches_ = false;
        release_was_long_press_ = false;
        // The menu may open between HID samples. Require a later, valid empty
        // sample before accepting another touch so a held finger cannot leak.
        blocked_until_release_ = had_active_touch;
        return {};
    }

    if (gesture_ == GestureState::ReleaseHold) {
        if (now < release_hold_until_) return currentState(true);
        gesture_ = GestureState::Idle;
        touches_ = {};
        had_multiple_touches_ = false;
        release_was_long_press_ = false;
        if (samples == 0) return {};
        if (state.count > 0) blocked_until_release_ = true;
        return {};
    }

    // A zero return means no HID sample was copied, not that all fingers were
    // lifted. Preserve an active touch until a valid sample reports count=0.
    if (samples == 0) {
        if (gesture_ == GestureState::Pending &&
            !had_multiple_touches_ &&
            now - gesture_started_ >= kLongPressDelay &&
            max_distance_squared_ <= kTapMaxDistance * kTapMaxDistance) {
            gesture_ = GestureState::LongPress;
        }
        if (gesture_ == GestureState::Pan) return currentState(false);
        if (gesture_ == GestureState::LongPress) return currentState(true);
        return {};
    }

    const size_t count = state.count;
    if (blocked_until_release_) {
        if (count == 0) blocked_until_release_ = false;
        else {
            bool blocked_finger_present = false;
            for (size_t i = 0; i < count; ++i) {
                if (state.touches[i].finger_id == blocked_finger_id_) {
                    blocked_finger_present = true;
                    break;
                }
            }
            // Some HID sequences do not provide a separate empty sample
            // between an interrupted touch and the next touch. If the old
            // finger is gone, this is a new gesture and the fence can be
            // removed without requiring a stream restart.
            if (blocked_finger_present) return {};
            blocked_until_release_ = false;
        }
    }

    const HidTouchState* primary = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (state.touches[i].finger_id == primary_finger_id_) {
            primary = &state.touches[i];
            break;
        }
    }

    if (gesture_ != GestureState::Idle && !primary) {
        if (gesture_ == GestureState::Pending || gesture_ == GestureState::LongPress) {
            if ((gesture_ == GestureState::Pending && !had_multiple_touches_ &&
                 now - gesture_started_ <= kTapMaxDuration &&
                 max_distance_squared_ <= kTapMaxDistance * kTapMaxDistance) ||
                gesture_ == GestureState::LongPress) {
                release_was_long_press_ = gesture_ == GestureState::LongPress;
                gesture_ = GestureState::ReleaseHold;
                release_hold_until_ = now + kReleaseHoldDuration;
                return currentState(true);
            }
            touches_ = {};
            gesture_ = GestureState::Idle;
            if (count > 0) blocked_until_release_ = true;
            return {};
        }
    }

    if (gesture_ == GestureState::Idle && count > 0) {
        primary = &state.touches[0];
        gesture_ = GestureState::Pending;
        release_was_long_press_ = false;
        primary_finger_id_ = primary->finger_id;
        max_distance_squared_ = 0;
        had_multiple_touches_ = count > 1;
        gesture_started_ = now;
    }

    if (gesture_ == GestureState::Idle) return {};
    updateTrackedTouches(state);
    if (activeTouchCount() == 0) {
        gesture_ = GestureState::Idle;
        return {};
    }

    if (activeTouchCount() > 1) had_multiple_touches_ = true;

    if (gesture_ == GestureState::Pan) return currentState(false);
    if (gesture_ == GestureState::LongPress) return currentState(true);
    if (!primary) return {};

    for (const auto& tracked : touches_) {
        if (!tracked.active) continue;
        const HidTouchState* input = nullptr;
        for (size_t i = 0; i < count; ++i) {
            if (state.touches[i].finger_id == tracked.finger_id) {
                input = &state.touches[i];
                break;
            }
        }
        if (!input) continue;
        const int64_t dx = static_cast<int64_t>(input->x) - tracked.down_x;
        const int64_t dy = static_cast<int64_t>(input->y) - tracked.down_y;
        const uint32_t distance_squared = static_cast<uint32_t>(dx * dx + dy * dy);
        max_distance_squared_ = std::max(max_distance_squared_, distance_squared);
    }

    if (gesture_ == GestureState::Pending) {
        if (max_distance_squared_ >= kPanMinDistance * kPanMinDistance) {
            gesture_ = GestureState::Pan;
        } else if (!had_multiple_touches_ &&
                   now - gesture_started_ >= kLongPressDelay &&
                   max_distance_squared_ <= kTapMaxDistance * kTapMaxDistance) {
            gesture_ = GestureState::LongPress;
        }
    }

    if (gesture_ == GestureState::LongPress) return currentState(true);
    return {};
}

void PsTouchpadReader::reset() {
    gesture_ = GestureState::Idle;
    touches_ = {};
    had_multiple_touches_ = false;
    release_was_long_press_ = false;
    // Reset is an empty input boundary, so the next valid touch is a new
    // gesture. There is no held touch to fence off here.
    blocked_until_release_ = false;
    blocked_finger_id_ = 0;
}

} // namespace lunar::ps

#endif
