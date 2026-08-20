#pragma once

#include "gamepad_reader.h"
#include "xinput_encoder.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace lunar::input {

class XboxInputAccumulator {
public:
    struct Batch {
        std::vector<GamepadState> frames;
        bool reliable = false;
        uint64_t last_transition_id = 0;
        uint64_t latest_generation = 0;
        bool includes_latest = false;
    };

    static constexpr size_t kMaxPendingTransitions = 256;

    void reset();
    void publish(const GamepadState& state,
                 bool delivery_ready,
                 bool mark_latest,
                 bool force_reliable = false);
    std::optional<Batch> peekBatch() const;
    void commitBatch(const Batch& batch);
    void prepareForReconnect();
    bool consumeOverflowFault();
    size_t pendingTransitionCount() const;

    static bool hasDigitalTransition(const GamepadState& previous,
                                     const GamepadState& current);
    static bool sameEncodedState(const GamepadState& left,
                                 const GamepadState& right);

private:
    struct Transition {
        uint64_t id = 0;
        GamepadState state{};
    };

    mutable std::mutex mutex_;
    std::deque<Transition> transitions_;
    GamepadState latest_state_{};
    GamepadState last_sampled_state_{};
    uint64_t next_transition_id_ = 1;
    uint64_t latest_generation_ = 0;
    bool latest_dirty_ = false;
    bool has_sampled_state_ = false;
    bool force_reliable_snapshot_ = false;
    bool overflow_fault_ = false;
};

} // namespace lunar::input
