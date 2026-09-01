#include "ps/ps_haptics_rumble.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace {

using lunar::ps::PsHapticsRumbleAccumulator;
using lunar::ps::PsHapticsRumbleCommand;

template <size_t N>
bool push(PsHapticsRumbleAccumulator& accumulator,
          const std::array<int16_t, N>& samples,
          PsHapticsRumbleCommand& command) {
    return accumulator.pushFrame(
        reinterpret_cast<const uint8_t*>(samples.data()),
        samples.size() * sizeof(samples[0]), command);
}

void aggregatesThreeStereoPacketsWithoutCollapsingChannels() {
    PsHapticsRumbleAccumulator accumulator;
    PsHapticsRumbleCommand command;
    const std::array<int16_t, 4> samples = {
        1000, -2000,
        -3000, 4000,
    };

    assert(!push(accumulator, samples, command));
    assert(!push(accumulator, samples, command));
    assert(push(accumulator, samples, command));
    assert(std::abs(command.left - 2000.0f / 32768.0f) < 0.00001f);
    assert(std::abs(command.right - 3000.0f / 32768.0f) < 0.00001f);
}

void silenceProducesAnExplicitStopCommand() {
    PsHapticsRumbleAccumulator accumulator;
    PsHapticsRumbleCommand command;
    const std::array<int16_t, 2> active = {12000, 16000};
    const std::array<int16_t, 2> silent = {0, 0};

    assert(!push(accumulator, active, command));
    assert(!push(accumulator, active, command));
    assert(push(accumulator, active, command));
    assert(command.left > 0.0f);
    assert(command.right > 0.0f);

    assert(!push(accumulator, silent, command));
    assert(!push(accumulator, silent, command));
    assert(push(accumulator, silent, command));
    assert(command.left == 0.0f);
    assert(command.right == 0.0f);
}

void handlesFullScaleNegativePcmWithoutOverflow() {
    PsHapticsRumbleAccumulator accumulator;
    PsHapticsRumbleCommand command;
    const std::array<int16_t, 2> samples = {-32768, -32768};

    assert(!push(accumulator, samples, command));
    assert(!push(accumulator, samples, command));
    assert(push(accumulator, samples, command));
    assert(command.left == 1.0f);
    assert(command.right == 1.0f);
}

void ignoresMalformedPacketsAndResetDropsPartialWindows() {
    PsHapticsRumbleAccumulator accumulator;
    PsHapticsRumbleCommand command;
    const std::array<int16_t, 2> samples = {8000, 4000};
    const std::array<uint8_t, 3> malformed = {0, 1, 2};

    assert(!accumulator.pushFrame(nullptr, 0, command));
    assert(!accumulator.pushFrame(malformed.data(), malformed.size(), command));
    assert(!push(accumulator, samples, command));
    assert(!push(accumulator, samples, command));
    accumulator.reset();
    assert(!push(accumulator, samples, command));
    assert(!push(accumulator, samples, command));
    assert(push(accumulator, samples, command));
}

} // namespace

int main() {
    aggregatesThreeStereoPacketsWithoutCollapsingChannels();
    silenceProducesAnExplicitStopCommand();
    handlesFullScaleNegativePcmWithoutOverflow();
    ignoresMalformedPacketsAndResetDropsPartialWindows();
    return 0;
}
