#pragma once

#ifdef __SWITCH__

#include <chiaki/controller.h>
#include "../input/gamepad_reader.h"

namespace lunar::ps {

class PsInputMapper {
public:
    PsInputMapper() = default;

    ChiakiControllerState map(const input::GamepadState& state);
    void reset();

private:
    ChiakiControllerState prev_{};
};

} // namespace lunar::ps

#endif
