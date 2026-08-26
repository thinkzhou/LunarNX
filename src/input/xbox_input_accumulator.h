#pragma once

#include "gamepad_reader.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace lunar::input {

class XboxInputAccumulator {
public:
    struct Snapshot {
        GamepadState state{};
        uint64_t generation = 0;
        uint64_t sampled_at_ns = 0;
        uint64_t transition_id = 0;
    };

    void reset();
    void publish(const GamepadState& state, bool delivery_ready);
    void publishAt(const GamepadState& state,
                   bool delivery_ready,
                   uint64_t sampled_at_ns);
    std::optional<Snapshot> peekLatest() const;
    std::optional<Snapshot> peekTransition(uint64_t now_ns);
    void commitLatest(const Snapshot& snapshot);
    void commitTransition(const Snapshot& snapshot);
    void prepareForReconnect();

private:
    static constexpr size_t kMaxPendingTransitions = 8;
    static constexpr uint64_t kTransitionLifetimeNs = 50'000'000;

    static bool hasDigitalTransition(const GamepadState& previous,
                                     const GamepadState& current);

    mutable std::mutex mutex_;
    GamepadState latest_state_{};
    uint64_t latest_generation_ = 0;
    uint64_t latest_sampled_at_ns_ = 0;
    bool latest_dirty_ = false;
    bool has_sampled_state_ = false;
    uint64_t next_transition_id_ = 1;
    std::deque<Snapshot> transitions_;
};

} // namespace lunar::input
