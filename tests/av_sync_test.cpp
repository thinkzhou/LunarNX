#include "stream/av_sync.h"

#include <cassert>
#include <cstdint>

using lunar::stream::AVSync;

int main() {
    AVSync sync;
    sync.start();
    sync.updateVideoPts(1'000'000'000ULL);
    sync.updateAudioPts(900'000'000ULL);
    assert(sync.getVideoDelayNs(1'000'000'000ULL) == 100'000'000LL);

    sync.updateAudioPts(1'300'000'001ULL);
    const auto timing = sync.getVideoTiming(1'000'000'000ULL);
    assert(timing.raw_delay_ns == -300'000'001LL);
    assert(timing.clamped_delay_ns == -200'000'000LL);
    assert(timing.master_pts_ns == 1'300'000'001ULL);
    assert(timing.using_audio_master);
    sync.reset();
    return 0;
}
