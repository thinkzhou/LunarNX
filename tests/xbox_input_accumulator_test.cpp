#include "input/xbox_input_accumulator.h"

#include <cassert>

using lunar::input::GamepadState;
using lunar::input::XboxInputAccumulator;

int main() {
    XboxInputAccumulator input;
    GamepadState neutral{};

    // Sampling starts before the input protocol is ready, but no gamepad
    // packet may overtake the metadata packet.
    input.publish(neutral, false);
    assert(!input.peekLatest());

    // Green-NX-style delivery publishes one complete current state on every
    // 8 ms tick, even when the state did not change.
    input.publish(neutral, true);
    auto first = input.peekLatest();
    assert(first && first->generation == 1);
    assert(first->sampled_at_ns > 0);
    assert(!first->state.a);
    auto same_pending = input.peekLatest();
    assert(same_pending && same_pending->generation == first->generation);
    input.commitLatest(*first);
    assert(!input.peekLatest());

    input.publish(neutral, true);
    auto periodic = input.peekLatest();
    assert(periodic && periodic->generation == 2);
    input.commitLatest(*periodic);

    // A newer release supersedes an uncommitted press. Committing the stale
    // generation must not hide that release from the owner thread.
    GamepadState down{};
    down.a = true;
    input.publish(down, true);
    auto stale_press = input.peekLatest();
    assert(stale_press && stale_press->state.a);
    input.publish(neutral, true);
    input.commitLatest(*stale_press);
    auto release = input.peekLatest();
    assert(release && !release->state.a);
    assert(release->generation > stale_press->generation);
    input.commitLatest(*release);
    assert(!input.peekLatest());

    // Every snapshot is absolute, including analog state.
    GamepadState stick{};
    stick.left_stick_x = 1234;
    stick.right_trigger = 65535;
    input.publish(stick, true);
    auto analog = input.peekLatest();
    assert(analog);
    assert(analog->state.left_stick_x == 1234);
    assert(analog->state.right_trigger == 65535);
    input.commitLatest(*analog);

    // Reconnect forces the last sampled state to be announced again without
    // manufacturing or replaying an intermediate transition.
    input.prepareForReconnect();
    auto resync = input.peekLatest();
    assert(resync);
    assert(resync->state.left_stick_x == 1234);
    assert(resync->generation > analog->generation);
    input.commitLatest(*resync);

    input.reset();
    assert(!input.peekLatest());

    // A bounded digital transition journal preserves a short tap even when
    // latest-state coalescing has already advanced to the release. Analog-only
    // movement never enters the journal.
    XboxInputAccumulator journal;
    journal.publishAt(neutral, true, 1'000'000);
    journal.publishAt(down, true, 9'000'000);
    journal.publishAt(neutral, true, 17'000'000);
    auto press_transition = journal.peekTransition(20'000'000);
    assert(press_transition && press_transition->state.a);
    journal.commitTransition(*press_transition);
    auto release_transition = journal.peekTransition(21'000'000);
    assert(release_transition && !release_transition->state.a);
    journal.commitTransition(*release_transition);
    assert(!journal.peekTransition(22'000'000));

    GamepadState analog_only = neutral;
    analog_only.left_stick_x = 1000;
    journal.publishAt(analog_only, true, 24'000'000);
    assert(!journal.peekTransition(25'000'000));

    journal.publishAt(down, true, 30'000'000);
    assert(!journal.peekTransition(80'000'001));
    return 0;
}
