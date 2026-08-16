#include "input/stream_input_router.h"

#include <cassert>

using lunar::input::GamepadState;
using lunar::input::StreamInputOwner;
using lunar::input::StreamInputRouter;

int main() {
    StreamInputRouter input_router;
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
    const auto resumed_game_input = input_router.route(menu_input);
    assert(resumed_game_input.a);
    assert(resumed_game_input.dpad_right);
    assert(resumed_game_input.right_trigger == 65535);
    return 0;
}
