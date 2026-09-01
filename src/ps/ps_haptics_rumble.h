#pragma once

#include <cstddef>
#include <cstdint>

namespace lunar::ps {

struct PsHapticsRumbleCommand {
    float left = 0.0f;
    float right = 0.0f;
};

// PS5 sends DualSense haptics as 10 ms stereo PCM packets. Accumulate the
// same three-packet window used by chiaki-ng before mapping its envelope to
// conventional left/right rumble motors.
class PsHapticsRumbleAccumulator {
public:
    bool pushFrame(const uint8_t* data, size_t size,
                   PsHapticsRumbleCommand& command);
    void reset();

private:
    static constexpr size_t kPacketsPerCommand = 3;

    uint64_t left_sum_ = 0;
    uint64_t right_sum_ = 0;
    uint64_t sample_count_ = 0;
    size_t packet_count_ = 0;
};

} // namespace lunar::ps
