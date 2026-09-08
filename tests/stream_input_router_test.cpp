#include "input/stream_input_router.h"

#include <cassert>

using lunar::input::GamepadState;
using lunar::input::StreamInputOwner;
using lunar::input::StreamInputRouter;

int main() {
    using lunar::input::MenuChordFilter;
    for (uint64_t first : {uint64_t{1}, uint64_t{2}}) {
        MenuChordFilter chord;
        assert(chord.update(first | 4, 3, 100) == 4);
        assert(chord.update(3 | 4, 3, 180) == 4);
        assert(chord.update(first, 3, 200) == 0); // Release one key first.
        assert(chord.update(0, 3, 210) == 0);
        assert(chord.update(0, 3, 260) == 0);
    }
    MenuChordFilter tap;
    assert(tap.update(1,3,100)==0);
    assert(tap.update(0,3,150)==0 && tap.replayedButtons()==1);
    assert(tap.update(0,3,180)==0 && tap.replayedButtons()==1);
    assert(tap.update(0,3,191)==0 && tap.replayedButtons()==0);
    MenuChordFilter hold;
    assert(hold.update(2,3,100)==0);
    assert(hold.update(2,3,220)==2);
    assert(hold.update(2,3,1000)==2);
    assert(hold.update(0,3,1001)==0);

    // A tap straddling the recognition threshold must still produce an edge.
    for (uint64_t release : {uint64_t{119}, uint64_t{120}, uint64_t{121}, uint64_t{128}, uint64_t{300}}) {
        MenuChordFilter delayed;
        assert(delayed.update(2,3,100)==0);
        assert(delayed.update(2,3,212)==0);
        assert(delayed.update(0,3,100+release)==0 && delayed.replayedButtons()==2);
        assert(delayed.update(0,3,141+release)==0 && delayed.replayedButtons()==0);
    }
    StreamInputRouter input_router(true);
    GamepadState menu_input;
    menu_input.a = true;
    menu_input.dpad_right = true;
    menu_input.right_trigger = 65535;

    input_router.setOwner(StreamInputOwner::Ui);
    const auto remote_menu_input = input_router.route(menu_input);
    assert(!remote_menu_input.a);
    assert(!remote_menu_input.dpad_right);
    assert(remote_menu_input.right_trigger == 0);

    input_router.setOwner(StreamInputOwner::Game);
    assert(!input_router.gameHasInput());
    assert(!input_router.route(menu_input).a);
    input_router.setOwner(StreamInputOwner::Game); // Repeated resume must not remove the fence.
    assert(!input_router.route(menu_input).a);
    GamepadState stick;
    stick.left_stick_x = 20000;
    assert(input_router.route(stick).left_stick_x == 0);
    assert(!input_router.gameHasInput());
    input_router.route(GamepadState{});
    assert(input_router.gameHasInput());
    const auto resumed_game_input = input_router.route(menu_input);
    assert(resumed_game_input.a);
    assert(resumed_game_input.dpad_right);
    assert(resumed_game_input.right_trigger == 65535);
    input_router.setOwner(StreamInputOwner::Ui);
    input_router.setOwner(StreamInputOwner::Game);
    GamepadState back;
    back.b = true;
    assert(!input_router.route(back).b);
    input_router.setOwner(StreamInputOwner::Ui);
    input_router.route(GamepadState{}); // Neutral while UI owns input cannot unlock the fence.
    input_router.setOwner(StreamInputOwner::Game);
    assert(!input_router.route(back).b);
    GamepadState drift;
    drift.left_stick_x = 1000;
    input_router.route(drift);
    assert(input_router.route(back).b);
    // Other runtimes retain their existing virtual-button routing contract.
    StreamInputRouter legacy;
    legacy.setOwner(StreamInputOwner::Ui);
    assert(!legacy.route(back).b);
    legacy.setOwner(StreamInputOwner::Game);
    assert(legacy.route(back).b);
    return 0;
}
