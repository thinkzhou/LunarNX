#pragma once

#include "../common.h"
#include "media_pipeline.h"
#include "video_decoder.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace lunar::stream {

struct PerfStats;

enum class RenderFault : uint8_t {
    None = 0,
    InvalidFrame,
    MissingNvMap,
    InvalidNvMap,
    MappingCacheExhausted,
    ExternalMemblockFailed,
    DescriptorUpdateFailed,
    FrameReferenceFailed,
    QueueError,
    CommandFenceTimeout,
};

inline const char* renderFaultName(RenderFault fault) {
    switch (fault) {
        case RenderFault::None: return "none";
        case RenderFault::InvalidFrame: return "invalid-frame";
        case RenderFault::MissingNvMap: return "missing-nvmap";
        case RenderFault::InvalidNvMap: return "invalid-nvmap";
        case RenderFault::MappingCacheExhausted: return "mapping-cache-exhausted";
        case RenderFault::ExternalMemblockFailed: return "external-memblock-failed";
        case RenderFault::DescriptorUpdateFailed: return "descriptor-update-failed";
        case RenderFault::FrameReferenceFailed: return "frame-reference-failed";
        case RenderFault::QueueError: return "queue-error";
        case RenderFault::CommandFenceTimeout: return "command-fence-timeout";
    }
    return "unknown";
}

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
    void setProgressSink(std::atomic<uint8_t>* stage,
                         std::atomic<uint64_t>* stage_started_ns);
    void setVideoBackend(VideoBackend backend);
    void setPresentationMode(VideoPresentationMode mode) {
        presentation_mode_.store(mode, std::memory_order_release);
    }
    void setPostProcessMode(PostProcessMode mode);
    void setPostProcessEnabled(bool enabled);
    void setDitheringEnabled(bool enabled, float strength = 3.0f);
    void setHoldNonTargetStartupFrames(bool enabled) {
        hold_non_target_startup_frames_ = enabled;
    }
    bool render(const VideoFrame& frame);
    void present();
    RenderFault consumeRenderFault();
    uint64_t successfulPresentCount() const {
        return successful_presents_.load(std::memory_order_acquire);
    }
    uint64_t lastSuccessfulPresentNs() const {
        return last_successful_present_ns_.load(std::memory_order_acquire);
    }
    uint32_t consecutiveRenderFaults() const {
        return consecutive_render_faults_.load(std::memory_order_acquire);
    }
    void resetLiveness();
    bool prepareDecoderReset();
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
    bool hold_non_target_startup_frames_ = false;
    std::atomic<VideoPresentationMode> presentation_mode_{
        VideoPresentationMode::BufferedFifo};
    std::atomic<uint8_t> pending_render_fault_{
        static_cast<uint8_t>(RenderFault::None)};
    std::atomic<uint64_t> successful_presents_{0};
    std::atomic<uint64_t> last_successful_present_ns_{0};
    std::atomic<uint32_t> consecutive_render_faults_{0};
    // A command fence that never retires makes queue.waitIdle() unsafe.  Keep
    // the old GPU-owned context alive rather than blocking or freeing memory
    // the GPU may still reference.
    std::atomic<bool> gpu_quarantine_required_{false};
    std::atomic<uint8_t>* progress_stage_sink_ = nullptr;
    std::atomic<uint64_t>* progress_stage_started_ns_sink_ = nullptr;

    void recordSuccessfulPresent();
    void markRenderFault(RenderFault fault);
    void setRenderStage(VideoRenderStage stage);
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
