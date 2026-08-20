#include "input/xbox_input_accumulator.h"

#include <cassert>

using lunar::input::GamepadState;
using lunar::input::XboxInputAccumulator;

int main() {
    XboxInputAccumulator input;
    GamepadState neutral{};
    input.publish(neutral, true, true);
    auto latest = input.peekBatch();
    assert(latest && latest->frames.size() == 1);
    input.commitBatch(*latest);

    // Reliable ordered input must not enqueue identical idle snapshots on
    // every producer tick; the session supplies an explicit heartbeat when
    // the pad has been unchanged long enough.
    input.publish(neutral, true, true);
    assert(!input.peekBatch());
    input.publish(neutral, true, false, true);
    auto heartbeat = input.peekBatch();
    assert(heartbeat && heartbeat->includes_latest);
    input.commitBatch(*heartbeat);

    GamepadState down{};
    down.a = true;
    input.publish(down, true, false);
    input.publish(neutral, true, true);
    auto transitions = input.peekBatch();
    assert(transitions);
    assert(transitions->frames.size() == 2);
    assert(transitions->frames[0].a);
    assert(!transitions->frames[1].a);

    auto retry = input.peekBatch();
    assert(retry && retry->frames.size() == transitions->frames.size());
    assert(retry->last_transition_id == transitions->last_transition_id);

    input.publish(down, true, false);
    input.commitBatch(*transitions);
    auto newer = input.peekBatch();
    assert(newer);
    assert(newer->frames.front().a);

    input.reset();
    input.publish(neutral, true, true);
    for (size_t i = 0; i < 35; ++i) {
        GamepadState state{};
        state.a = (i % 2) == 0;
        input.publish(state, true, false);
    }
    auto first = input.peekBatch();
    assert(first);
    assert(first->frames.size() == 29);
    assert(!first->includes_latest);
    input.commitBatch(*first);
    auto second = input.peekBatch();
    assert(second);
    assert(second->frames.size() == 6);

    input.reset();
    input.publish(neutral, true, false);
    GamepadState last_transition{};
    for (size_t i = 0; i < 29; ++i) {
        last_transition.a = (i % 2) == 0;
        input.publish(last_transition, true, false);
    }
    input.publish(last_transition, true, false, true);
    auto full_transition_batch = input.peekBatch();
    assert(full_transition_batch);
    assert(full_transition_batch->frames.size() == 29);
    assert(!full_transition_batch->includes_latest);
    input.commitBatch(*full_transition_batch);
    auto forced_after_full_batch = input.peekBatch();
    assert(forced_after_full_batch);
    assert(forced_after_full_batch->includes_latest);

    input.reset();
    input.publish(neutral, true, false);
    input.publish(down, true, false);
    auto old_reliable_batch = input.peekBatch();
    assert(old_reliable_batch);
    GamepadState forced_new_generation = down;
    forced_new_generation.left_stick_x = 9000;
    input.publish(forced_new_generation, true, false, true);
    input.commitBatch(*old_reliable_batch);
    auto forced_after_old_commit = input.peekBatch();
    assert(forced_after_old_commit);
    assert(forced_after_old_commit->includes_latest);

    input.reset();
    input.publish(neutral, true, true);
    input.commitBatch(*input.peekBatch());
    GamepadState stick{};
    stick.left_stick_x = 1234;
    input.publish(stick, true, true);
    auto analog = input.peekBatch();
    assert(analog);

    input.commitBatch(*analog);
    input.publish(neutral, true, false, true);
    auto ui_neutral = input.peekBatch();
    assert(ui_neutral);
    input.commitBatch(*ui_neutral);

    input.prepareForReconnect();
    auto resync = input.peekBatch();
    assert(resync && resync->includes_latest);

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
