#include "ps_haptics_rumble.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace lunar::ps {
namespace {

constexpr uint32_t kHapticsNoiseFloor = 100;
constexpr float kPcmScale = 32768.0f;

uint32_t pcmMagnitude(int16_t sample) {
    if (sample == std::numeric_limits<int16_t>::min()) return 32768;
    return static_cast<uint32_t>(sample < 0 ? -sample : sample);
}

float normalizedMagnitude(uint64_t sum, uint64_t count) {
    if (count == 0) return 0.0f;
    const uint64_t average = sum / count;
    if (average <= kHapticsNoiseFloor) return 0.0f;
    return std::min(static_cast<float>(average) / kPcmScale, 1.0f);
}

} // namespace

bool PsHapticsRumbleAccumulator::pushFrame(
    const uint8_t* data, size_t size, PsHapticsRumbleCommand& command) {
    constexpr size_t kStereoSampleBytes = 2 * sizeof(int16_t);
    if (!data || size == 0 || size % kStereoSampleBytes != 0) return false;

    for (size_t offset = 0; offset < size; offset += kStereoSampleBytes) {
        int16_t left = 0;
        int16_t right = 0;
        std::memcpy(&left, data + offset, sizeof(left));
        std::memcpy(&right, data + offset + sizeof(left), sizeof(right));
        left_sum_ += pcmMagnitude(left);
        right_sum_ += pcmMagnitude(right);
        sample_count_++;
    }
    packet_count_++;
    if (packet_count_ < kPacketsPerCommand) return false;

    command.left = normalizedMagnitude(left_sum_, sample_count_);
    command.right = normalizedMagnitude(right_sum_, sample_count_);
    reset();
    return true;
}

void PsHapticsRumbleAccumulator::reset() {
    left_sum_ = 0;
    right_sum_ = 0;
    sample_count_ = 0;
    packet_count_ = 0;
}

} // namespace lunar::ps
