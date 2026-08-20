#include "input/xbox_input_accumulator.h"

#include <cassert>

using lunar::input::GamepadState;
using lunar::input::XboxInputAccumulator;

int main() {
    XboxInputAccumulator input;
    GamepadState neutral{};
    input.publish(neutral, true, true);
    auto latest = input.peekBatch();
    assert(latest && !latest->reliable);
    input.commitBatch(*latest);

    GamepadState down{};
    down.a = true;
    input.publish(down, true, false);
    input.publish(neutral, true, true);
    auto transitions = input.peekBatch();
    assert(transitions && transitions->reliable);
    assert(transitions->frames.size() == 2);
    assert(transitions->frames[0].a);
    assert(!transitions->frames[1].a);

    auto retry = input.peekBatch();
    assert(retry && retry->frames.size() == transitions->frames.size());
    assert(retry->last_transition_id == transitions->last_transition_id);

    input.publish(down, true, false);
    input.commitBatch(*transitions);
    auto newer = input.peekBatch();
    assert(newer && newer->reliable);
    assert(newer->frames.front().a);

    input.reset();
    input.publish(neutral, true, true);
    for (size_t i = 0; i < 35; ++i) {
        GamepadState state{};
        state.a = (i % 2) == 0;
        input.publish(state, true, false);
    }
    auto first = input.peekBatch();
    assert(first && first->reliable);
    assert(first->frames.size() == 29);
    assert(!first->includes_latest);
    input.commitBatch(*first);
    auto second = input.peekBatch();
    assert(second && second->reliable);
    assert(second->frames.size() == 6);

    input.reset();
    input.publish(neutral, true, true);
    input.commitBatch(*input.peekBatch());
    GamepadState stick{};
    stick.left_stick_x = 1234;
    input.publish(stick, true, true);
    auto analog = input.peekBatch();
    assert(analog && !analog->reliable);

    input.commitBatch(*analog);
    input.publish(neutral, true, false, true);
    auto ui_neutral = input.peekBatch();
    assert(ui_neutral && ui_neutral->reliable);
    input.commitBatch(*ui_neutral);

    input.prepareForReconnect();
    auto resync = input.peekBatch();
    assert(resync && resync->reliable && resync->includes_latest);

    input.reset();
    input.publish(neutral, true, false);
    for (size_t i = 0;
         i <= XboxInputAccumulator::kMaxPendingTransitions; ++i) {
        GamepadState state{};
        state.a = (i % 2) == 0;
        input.publish(state, true, false);
    }
    assert(input.pendingTransitionCount() ==
           XboxInputAccumulator::kMaxPendingTransitions);
    assert(input.consumeOverflowFault());
    assert(!input.consumeOverflowFault());
    return 0;
}
