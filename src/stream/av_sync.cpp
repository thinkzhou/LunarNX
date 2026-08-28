#include "av_sync.h"
#include <chrono>
#include <algorithm>

namespace lunar::stream {

AVSync::AVSync() = default;
AVSync::~AVSync() = default;

void AVSync::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    base_time_ = std::chrono::steady_clock::now();
    video_pts_ = 0;
    audio_pts_ = 0;
    first_video_pts_ = 0;
    have_video_base_ = false;
    have_audio_base_ = false;
    running_ = true;
}

void AVSync::updateVideoPts(uint64_t pts_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    if (!have_video_base_) {
        first_video_pts_ = pts_ns;
        base_time_ = std::chrono::steady_clock::now();
        have_video_base_ = true;
    }
    video_pts_ = pts_ns;
}

void AVSync::updateAudioPts(uint64_t pts_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    if (!have_audio_base_) {
        have_audio_base_ = true;
    }
    audio_pts_ = pts_ns;
    last_audio_time_ = std::chrono::steady_clock::now();
}

void AVSync::invalidateAudioClock() {
    std::lock_guard<std::mutex> lock(mutex_);
    audio_pts_ = 0;
    have_audio_base_ = false;
}

AVSyncTiming AVSync::getVideoTiming(uint64_t frame_pts) const {
    std::lock_guard<std::mutex> lock(mutex_);
    AVSyncTiming timing;
    timing.frame_pts_ns = frame_pts;
    if (!running_ || !have_video_base_) return timing;

    // Use audio as the master clock when available, else wall clock.
    // Fall back to wall clock if audio hasn't updated in >500ms (stall).
    auto now = std::chrono::steady_clock::now();
    bool audio_stale = !have_audio_base_;
    if (have_audio_base_) {
        timing.audio_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_audio_time_).count();
        audio_stale = timing.audio_age_ms > 500;
    }

    int64_t master_pts;
    if (!audio_stale) {
        master_pts = static_cast<int64_t>(audio_pts_);
        timing.using_audio_master = true;
    } else {
        master_pts = static_cast<int64_t>(first_video_pts_) +
            static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - base_time_).count());
    }

    timing.master_pts_ns = master_pts > 0
        ? static_cast<uint64_t>(master_pts)
        : 0;

    int64_t pts = static_cast<int64_t>(frame_pts);
    int64_t delay = pts - master_pts;
    timing.raw_delay_ns = delay;

    if (delay < -200'000'000LL) delay = -200'000'000LL;
    if (delay > 200'000'000LL) delay = 200'000'000LL;

    timing.clamped_delay_ns = delay;
    return timing;
}

int64_t AVSync::getVideoDelayNs(uint64_t frame_pts) const {
    return getVideoTiming(frame_pts).clamped_delay_ns;
}

void AVSync::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    video_pts_ = 0;
    audio_pts_ = 0;
    first_video_pts_ = 0;
    have_video_base_ = false;
    have_audio_base_ = false;
}

} // namespace lunar::stream
