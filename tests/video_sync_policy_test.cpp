#include "stream/video_sync_policy.h"

#include <assert.h>

int main() {
    using lunar::stream::VideoSyncAction;
    using lunar::stream::videoSyncAction;
    using lunar::stream::videoSyncWaitNs;

    assert(videoSyncAction(68'000'000LL) == VideoSyncAction::Wait);
    assert(videoSyncWaitNs(68'000'000LL) == 68'000'000LL);
    assert(videoSyncAction(50'000'000LL) == VideoSyncAction::Render);
    assert(videoSyncAction(-200'000'000LL) == VideoSyncAction::Render);
    assert(videoSyncAction(-200'000'001LL) == VideoSyncAction::Drop);
    assert(videoSyncWaitNs(-1) == 0);
    return 0;
}
