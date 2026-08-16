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
    void setOwner(StreamInputOwner owner) {
        owner_.store(owner);
    }

    StreamInputOwner owner() const {
        return owner_.load();
    }

    bool gameHasInput() const {
        return owner() == StreamInputOwner::Game;
    }

    GamepadState route(const GamepadState& state) const {
        return gameHasInput() ? state : GamepadState{};
    }

private:
    std::atomic<StreamInputOwner> owner_{StreamInputOwner::Game};
};

} // namespace lunar::input
