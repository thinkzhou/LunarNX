#include "xbox_input_accumulator.h"

#include <algorithm>

namespace lunar::input {

void XboxInputAccumulator::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    transitions_.clear();
    latest_state_ = {};
    last_sampled_state_ = {};
    next_transition_id_ = 1;
    latest_generation_ = 0;
    latest_dirty_ = false;
    has_sampled_state_ = false;
    force_reliable_snapshot_ = false;
    overflow_fault_ = false;
}

void XboxInputAccumulator::publish(const GamepadState& state,
                                   bool delivery_ready,
                                   bool mark_latest,
                                   bool force_reliable) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool transition = has_sampled_state_ &&
                            hasDigitalTransition(last_sampled_state_, state);
    if (transition && delivery_ready) {
        if (transitions_.size() >= kMaxPendingTransitions) {
            overflow_fault_ = true;
        } else {
            transitions_.push_back({next_transition_id_++, state});
        }
    }
    latest_state_ = state;
    last_sampled_state_ = state;
    has_sampled_state_ = true;
    if (transition || mark_latest || force_reliable) {
        latest_dirty_ = true;
        ++latest_generation_;
    }
    if (force_reliable) {
        force_reliable_snapshot_ = true;
    }
}

std::optional<XboxInputAccumulator::Batch> XboxInputAccumulator::peekBatch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_sampled_state_ ||
        (transitions_.empty() && !latest_dirty_ && !force_reliable_snapshot_)) {
        return std::nullopt;
    }

    Batch batch;
    batch.reliable = !transitions_.empty() || force_reliable_snapshot_;
    const size_t transition_count = std::min(
        transitions_.size(), XInputEncoder::kMaxGamepadFrames);
    batch.frames.reserve(XInputEncoder::kMaxGamepadFrames);
    for (size_t i = 0; i < transition_count; ++i) {
        batch.frames.push_back(transitions_[i].state);
        batch.last_transition_id = transitions_[i].id;
    }

    const bool included_all_transitions = transition_count == transitions_.size();
    if (included_all_transitions &&
        batch.frames.size() < XInputEncoder::kMaxGamepadFrames &&
        (latest_dirty_ || force_reliable_snapshot_)) {
        if (batch.frames.empty() ||
            !sameEncodedState(batch.frames.back(), latest_state_)) {
            batch.frames.push_back(latest_state_);
        }
        batch.includes_latest = true;
        batch.latest_generation = latest_generation_;
    }
    return batch.frames.empty() ? std::nullopt
                                : std::optional<Batch>(std::move(batch));
}

void XboxInputAccumulator::commitBatch(const Batch& batch) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!transitions_.empty() &&
           transitions_.front().id <= batch.last_transition_id) {
        transitions_.pop_front();
    }
    if (batch.includes_latest &&
        latest_generation_ == batch.latest_generation) {
        latest_dirty_ = false;
        force_reliable_snapshot_ = false;
    }
}

void XboxInputAccumulator::prepareForReconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    transitions_.clear();
    if (has_sampled_state_) {
        latest_dirty_ = true;
        ++latest_generation_;
        force_reliable_snapshot_ = true;
    }
    overflow_fault_ = false;
}

bool XboxInputAccumulator::consumeOverflowFault() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool fault = overflow_fault_;
    overflow_fault_ = false;
    return fault;
}

size_t XboxInputAccumulator::pendingTransitionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transitions_.size();
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
           previous.guide != current.guide;
}

bool XboxInputAccumulator::sameEncodedState(const GamepadState& left,
                                            const GamepadState& right) {
    return left.a == right.a && left.b == right.b &&
           left.x == right.x && left.y == right.y &&
           left.dpad_up == right.dpad_up &&
           left.dpad_down == right.dpad_down &&
           left.dpad_left == right.dpad_left &&
           left.dpad_right == right.dpad_right &&
           left.lb == right.lb && left.rb == right.rb &&
           left.lt == right.lt && left.rt == right.rt &&
           left.l3 == right.l3 && left.r3 == right.r3 &&
           left.view == right.view && left.menu == right.menu &&
           left.guide == right.guide &&
           left.left_stick_x == right.left_stick_x &&
           left.left_stick_y == right.left_stick_y &&
           left.right_stick_x == right.right_stick_x &&
           left.right_stick_y == right.right_stick_y &&
           left.left_trigger == right.left_trigger &&
           left.right_trigger == right.right_trigger;
}

} // namespace lunar::input
