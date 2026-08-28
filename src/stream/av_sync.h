#pragma once

#include "../common.h"
#include <chrono>
#include <mutex>

namespace lunar::stream {

struct AVSyncTiming {
    int64_t raw_delay_ns = 0;
    int64_t clamped_delay_ns = 0;
    uint64_t frame_pts_ns = 0;
    uint64_t master_pts_ns = 0;
    int64_t audio_age_ms = -1;
    bool using_audio_master = false;
};

class AVSync {
public:
    AVSync();
    ~AVSync();

    void start();
    void updateVideoPts(uint64_t pts_ns);
    void updateAudioPts(uint64_t pts_ns);
    void invalidateAudioClock();
    AVSyncTiming getVideoTiming(uint64_t frame_pts) const;
    int64_t getVideoDelayNs(uint64_t frame_pts) const;
    void reset();

private:
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point base_time_;
    std::chrono::steady_clock::time_point last_audio_time_;
    uint64_t video_pts_ = 0;
    uint64_t audio_pts_ = 0;
    uint64_t first_video_pts_ = 0;
    bool have_video_base_ = false;
    bool have_audio_base_ = false;
    bool running_ = false;
};

} // namespace lunar::stream
