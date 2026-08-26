#include "xbox_input_accumulator.h"

#include <chrono>

namespace lunar::input {

void XboxInputAccumulator::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_state_ = {};
    latest_generation_ = 0;
    latest_sampled_at_ns_ = 0;
    latest_dirty_ = false;
    has_sampled_state_ = false;
    next_transition_id_ = 1;
    transitions_.clear();
}

void XboxInputAccumulator::publish(const GamepadState& state,
                                   bool delivery_ready) {
    const uint64_t sampled_at_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    publishAt(state, delivery_ready, sampled_at_ns);
}

void XboxInputAccumulator::publishAt(const GamepadState& state,
                                     bool delivery_ready,
                                     uint64_t sampled_at_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (delivery_ready && has_sampled_state_ &&
        hasDigitalTransition(latest_state_, state)) {
        if (transitions_.size() >= kMaxPendingTransitions) {
            transitions_.pop_front();
        }
        Snapshot transition;
        transition.state = state;
        transition.sampled_at_ns = sampled_at_ns;
        transition.transition_id = next_transition_id_++;
        transitions_.push_back(transition);
    }
    latest_state_ = state;
    latest_sampled_at_ns_ = sampled_at_ns;
    has_sampled_state_ = true;
    // Match Green-NX: every 8 ms sample becomes a fresh complete-state packet.
    // The owner thread may overwrite an older unsent packet, but it must never
    // replay a stale press after a newer release has been sampled.
    if (delivery_ready) {
        latest_dirty_ = true;
        ++latest_generation_;
    }
}

std::optional<XboxInputAccumulator::Snapshot>
XboxInputAccumulator::peekLatest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_sampled_state_ || !latest_dirty_) {
        return std::nullopt;
    }
    return Snapshot{latest_state_, latest_generation_, latest_sampled_at_ns_, 0};
}

std::optional<XboxInputAccumulator::Snapshot>
XboxInputAccumulator::peekTransition(uint64_t now_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!transitions_.empty()) {
        const auto& transition = transitions_.front();
        if (now_ns >= transition.sampled_at_ns &&
            now_ns - transition.sampled_at_ns > kTransitionLifetimeNs) {
            transitions_.pop_front();
            continue;
        }
        return transition;
    }
    return std::nullopt;
}

void XboxInputAccumulator::commitTransition(const Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!transitions_.empty() && snapshot.transition_id != 0 &&
        transitions_.front().transition_id == snapshot.transition_id) {
        transitions_.pop_front();
    }
}

void XboxInputAccumulator::commitLatest(const Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_generation_ == snapshot.generation) {
        latest_dirty_ = false;
    }
}

void XboxInputAccumulator::prepareForReconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    // A transition sampled for the old SCTP association is stale by the time
    // the new association is ready. Resync with only the current full state.
    transitions_.clear();
    if (has_sampled_state_) {
        latest_dirty_ = true;
        ++latest_generation_;
    }
}

bool XboxInputAccumulator::hasDigitalTransition(
    const GamepadState& previous,
    const GamepadState& current) {
    return previous.a != current.a || previous.b != current.b ||
           previous.x != current.x || previous.y != current.y ||
           previous.dpad_up != current.dpad_up ||
           previous.dpad_down != current.dpad_down ||
           previous.dpad_left != current.dpad_left ||
           previous.dpad_right != current.dpad_right ||
           previous.lb != current.lb || previous.rb != current.rb ||
           previous.lt != current.lt || previous.rt != current.rt ||
           previous.l3 != current.l3 || previous.r3 != current.r3 ||
           previous.view != current.view || previous.menu != current.menu ||
           previous.guide != current.guide ||
           previous.touchpad != current.touchpad;
}

} // namespace lunar::input
