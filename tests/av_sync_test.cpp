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
    sync.reset();
    return 0;
}
