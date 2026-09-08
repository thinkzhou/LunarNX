#pragma once

#include "gamepad_reader.h"
#include <atomic>

namespace lunar::input {

enum class StreamInputOwner {
    Game,
    Ui,
};

// Single source of truth for whether physical controller input belongs to the
// streamed game or to LunarNX's in-stream UI.
class StreamInputRouter {
public:
    // Enable for runtimes that route physical state before injecting virtual buttons.
    explicit StreamInputRouter(bool wait_for_release = false)
        : wait_for_release_(wait_for_release) {}

    void setOwner(StreamInputOwner owner) {
        if (owner == StreamInputOwner::Ui) {
            // Advance the generation so an old neutral sample cannot unlock a new UI visit.
            auto previous = state_.load();
            while (!state_.compare_exchange_weak(previous, ((previous + 4) & ~uint64_t{3}) | kUi)) {}
        } else {
            auto previous = state_.load();
            while ((previous & 3) == kUi &&
                   !state_.compare_exchange_weak(previous, (previous & ~uint64_t{3}) | (wait_for_release_ ? kAwaitRelease : kGame))) {}
        }
    }

    StreamInputOwner owner() const {
        return (state_.load() & 3) == kUi ? StreamInputOwner::Ui : StreamInputOwner::Game;
    }

    bool gameHasInput() const {
        return (state_.load() & 3) == kGame;
    }

    GamepadState route(const GamepadState& state) const {
        auto snapshot = state_.load();
        if ((snapshot & 3) == kGame) return state;
        if ((snapshot & 3) == kAwaitRelease && neutral(state)) {
            state_.compare_exchange_strong(snapshot, (snapshot & ~uint64_t{3}) | kGame);
        }
        return {};
    }

private:
    static constexpr uint64_t kGame = 0, kUi = 1, kAwaitRelease = 2;
    static bool neutral(const GamepadState& s) {
        // Allow normal stick drift, but do not forward UI navigation or held triggers.
        const auto centered = [](int v) { return v > -8000 && v < 8000; };
        return !(s.a || s.b || s.x || s.y || s.dpad_up || s.dpad_down ||
            s.dpad_left || s.dpad_right || s.lb || s.rb || s.lt || s.rt ||
            s.l3 || s.r3 || s.view || s.menu || s.guide || s.touchpad) &&
            centered(s.left_stick_x) && centered(s.left_stick_y) &&
            centered(s.right_stick_x) && centered(s.right_stick_y) &&
            s.left_trigger < 8000 && s.right_trigger < 8000;
    }
    const bool wait_for_release_;
    mutable std::atomic<uint64_t> state_{kGame};
};

} // namespace lunar::input
