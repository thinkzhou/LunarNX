#include "input/rumble_controller.h"

#include <cassert>

using lunar::input::RumblePhase;
using lunar::input::evaluateRumblePhase;

int main() {
    assert(evaluateRumblePhase(0, 20, 10, 2) == RumblePhase::Off);
    assert(evaluateRumblePhase(9, 20, 10, 2) == RumblePhase::Off);
    assert(evaluateRumblePhase(10, 20, 10, 2) == RumblePhase::On);
    assert(evaluateRumblePhase(29, 20, 10, 2) == RumblePhase::On);
    assert(evaluateRumblePhase(30, 20, 10, 2) == RumblePhase::Off);
    assert(evaluateRumblePhase(40, 20, 10, 2) == RumblePhase::On);
    assert(evaluateRumblePhase(60, 20, 10, 2) == RumblePhase::Off);
    assert(evaluateRumblePhase(70, 20, 10, 2) == RumblePhase::On);
    assert(evaluateRumblePhase(90, 20, 10, 2) == RumblePhase::Finished);

    // Match XStreaming's native fallback for a non-zero command with no
    // explicit duration: produce a short 30 ms pulse rather than dropping it.
    assert(evaluateRumblePhase(0, 0, 0, 0) == RumblePhase::On);
    assert(evaluateRumblePhase(29, 0, 0, 0) == RumblePhase::On);
    assert(evaluateRumblePhase(30, 0, 0, 0) == RumblePhase::Finished);
    return 0;
}
