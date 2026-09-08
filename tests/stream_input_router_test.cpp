#include "input/stream_input_router.h"

#include <cassert>

using lunar::input::GamepadState;
using lunar::input::StreamInputOwner;
using lunar::input::StreamInputRouter;

int main() {
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
