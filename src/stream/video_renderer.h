#pragma once

#include "../common.h"
#include "media_pipeline.h"
#include "video_decoder.h"
#include <mutex>
#include <vector>

namespace lunar::stream {

struct PerfStats;

/// Video renderer.
/// Switch: deko3d NV12→RGB with BT.709 matrix, zero-copy NVDEC→GPU.
/// Desktop: SDL2 with YUV hardware texture conversion.
///
/// Thread safety model (matching Moonlight-Switch):
///   render()  — called from stream/decoder thread, builds cmdlist
///   present() — called from borealis main thread, submits to deko3d queue
class VideoRenderer {
public:
    VideoRenderer();
    ~VideoRenderer();

    bool initialize(const char* title = "LunarNX", int width = 1280, int height = 720);
    void setPerfStats(PerfStats* stats) { perf_ = stats; }
    void setVideoBackend(VideoBackend backend);
    void setPostProcessMode(PostProcessMode mode);
    void setPostProcessEnabled(bool enabled);
    void setDitheringEnabled(bool enabled, float strength = 3.0f);
    bool render(const VideoFrame& frame);
    void present();
    bool pollEvents();
    void shutdown();

private:
    PerfStats* perf_ = nullptr;
    VideoBackend video_backend_ =
#ifdef __SWITCH__
        VideoBackend::HardwareZeroCopy;
#else
        VideoBackend::Software;
#endif
    PostProcessMode requested_post_process_mode_ = PostProcessMode::Off;
    bool requested_dithering_enabled_ = false;
    float requested_dithering_strength_ = 3.0f;
#ifdef __SWITCH__
    void* ctx_ = nullptr;  // Deko3DContext* (Switch) or unused (Desktop)
    void* software_sws_ = nullptr;
    std::vector<uint8_t> software_rgba_;
    std::mutex software_mutex_;
#else
    void* window_ = nullptr;
    void* renderer_ = nullptr;
    void* texture_ = nullptr;
    std::mutex sdl_mutex_;
#endif
};

} // namespace lunar::stream
