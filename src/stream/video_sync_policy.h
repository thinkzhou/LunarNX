#pragma once

#include <stdint.h>

namespace lunar::stream {

constexpr int64_t kVideoSyncMaxLeadNs = 50'000'000LL;
constexpr int64_t kVideoSyncMaxLagNs = -200'000'000LL;
constexpr int64_t kVideoSyncMaxWaitNs = 200'000'000LL;

enum class VideoSyncAction {
    Render,
    Wait,
    Drop,
};

constexpr VideoSyncAction videoSyncAction(int64_t delay_ns) {
    if (delay_ns < kVideoSyncMaxLagNs) return VideoSyncAction::Drop;
    if (delay_ns > kVideoSyncMaxLeadNs) return VideoSyncAction::Wait;
    return VideoSyncAction::Render;
}

constexpr int64_t videoSyncWaitNs(int64_t delay_ns) {
    if (videoSyncAction(delay_ns) != VideoSyncAction::Wait) return 0;
    return delay_ns < kVideoSyncMaxWaitNs ? delay_ns : kVideoSyncMaxWaitNs;
}

} // namespace lunar::stream
