#include "stream/audio_timing.h"

#include <cassert>
#include <cstdint>

using lunar::stream::estimateAudioPlaybackTimestamp;

int main() {
    assert(estimateAudioPlaybackTimestamp(
        1'000'000'000ULL, 960, 48000, 960) == 1'000'000'000ULL);
    assert(estimateAudioPlaybackTimestamp(
        1'000'000'000ULL, 960, 48000, 480) == 1'010'000'000ULL);
    assert(estimateAudioPlaybackTimestamp(
        10'000'000ULL, 960, 48000, 9600) == 0);
    return 0;
}
