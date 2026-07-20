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

int64_t AVSync::getVideoDelayNs(uint64_t frame_pts) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !have_video_base_) return 0;

    // Use audio as the master clock when available, else wall clock.
    // Fall back to wall clock if audio hasn't updated in >500ms (stall).
    auto now = std::chrono::steady_clock::now();
    bool audio_stale = !have_audio_base_;
    if (have_audio_base_) {
        auto audio_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_audio_time_).count();
        audio_stale = audio_age_ms > 500;
    }

    int64_t master_pts;
    if (!audio_stale) {
        master_pts = static_cast<int64_t>(audio_pts_);
    } else {
        master_pts = static_cast<int64_t>(first_video_pts_) +
            static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - base_time_).count());
    }

    int64_t pts = static_cast<int64_t>(frame_pts);
    int64_t delay = pts - master_pts;

    if (delay < -200'000'000LL) delay = -200'000'000LL;
    if (delay > 200'000'000LL) delay = 200'000'000LL;

    return delay;
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
