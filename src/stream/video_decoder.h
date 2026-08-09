#pragma once

#include "../common.h"
#include "media_pipeline.h"
#include <functional>
#include <cstdint>
#include <vector>

struct AVFrame;         // FFmpeg forward decl
struct AVBufferRef;     // FFmpeg hw device context

namespace lunar::stream {

struct VideoFrame {
    // CPU-accessible data (desktop software decode)
    uint8_t* data[4] = {};
    int linesize[4] = {};

    // GPU frame (Switch NVDEC zero-copy)
    // Set when format == AV_PIX_FMT_NVTEGRA.
    // Renderer extracts NvMap handles via av_nvtegra_frame_get_fbuf_map().
    AVFrame* avframe = nullptr;

    int width = 0;
    int height = 0;
    int format = 0;       // AV_PIX_FMT_YUV420P or AV_PIX_FMT_NVTEGRA
    uint64_t timestamp = 0;
};

class VideoDecoder {
public:
    using FrameCallback = std::function<void(const VideoFrame& frame)>;

    VideoDecoder();
    ~VideoDecoder();

    /// Initialize decoder. On Switch uses NVDEC hardware acceleration.
    /// On desktop uses standard FFmpeg H.264 software decode.
    bool initialize(int width = 1280, int height = 720);
    void setVideoBackend(VideoBackend backend) { video_backend_ = backend; }

    /// Decode a chunk of H.264 data. Callback fires for each complete frame.
    bool decode(const uint8_t* data, size_t len, uint64_t timestamp);

    void setCallback(FrameCallback cb);
    void setPerfStats(struct PerfStats* stats) { perf_ = stats; }
    /// Discard decoder reference state and wait for the next IDR.  This is
    /// used after RTP queue loss or a hardware decode error.
    bool resetForKeyframe();
    void flush();
    void shutdown();

private:
    bool deliverFrame(struct AVFrame* frame,
                      uint64_t timestamp,
                      int log_index,
                      bool log);

#ifdef __SWITCH__
    VideoBackend video_backend_ = VideoBackend::HardwareZeroCopy;
#else
    VideoBackend video_backend_ = VideoBackend::Software;
#endif
    void* codec_ctx_ = nullptr;
    void* parser_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;
    FrameCallback on_frame_;
    struct PerfStats* perf_ = nullptr;
    bool initialized_ = false;
    bool h264_decoder_ready_ = false;
    bool h264_seen_sps_ = false;
    bool h264_seen_pps_ = false;
    std::vector<uint8_t> h264_parameter_sets_;
    bool h264_parameter_sets_pending_ = false;
    int h264_wait_log_count_ = 0;
    int h264_error_log_count_ = 0;
};

} // namespace lunar::stream
