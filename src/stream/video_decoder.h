#pragma once

#include "../common.h"
#include "media_pipeline.h"
#include "video_codec.h"
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

/// Shared FFmpeg/NVDEC plumbing. Owns no path-specific policy; Xbox and
/// PlayStation decode strategies live in XboxVideoDecoder / PsVideoDecoder.
class VideoDecoder {
public:
    using FrameCallback = std::function<void(const VideoFrame& frame)>;

    VideoDecoder();
    virtual ~VideoDecoder();

    /// Initialize decoder. On Switch uses NVDEC hardware acceleration.
    /// On desktop uses the selected standard FFmpeg software decoder.
    /// Subclasses reset their own policy state before delegating here.
    virtual bool initialize(int width = 1280, int height = 720);
    void setVideoBackend(VideoBackend backend) { video_backend_ = backend; }
    void setVideoCodec(VideoCodec codec) { video_codec_ = codec; }

    /// Inspect one complete Annex-B access unit for queue-time metadata.
    /// Each path owns its parser choice.
    virtual VideoAccessUnitInfo inspectAccessUnit(const uint8_t* data,
                                                  size_t len) const = 0;

    /// Decode one complete Annex-B access unit.
    virtual bool decode(const uint8_t* data, size_t len, uint64_t timestamp,
                        const VideoAccessUnitInfo* inspected_access_unit = nullptr) = 0;

    void setCallback(FrameCallback cb);
    void setPerfStats(struct PerfStats* stats) { perf_ = stats; }
    /// Discard decoder reference state and wait for the next IDR.  This is
    /// used after RTP queue loss or a hardware decode error.
    virtual bool resetForKeyframe() = 0;
    void flush();
    void shutdown();

protected:
    /// FFmpeg send/receive loop shared by both paths (hardware NVDEC on
    /// Switch, software parser/decoder on desktop). No path policy here.
    bool decodeAccessUnit(const uint8_t* data, size_t len, uint64_t timestamp,
                          const VideoAccessUnitInfo& au, int log_index, bool log);
    /// avcodec_flush_buffers + parser rebuild, shared by resetForKeyframe().
    bool reinitializeParser();
    /// Hand a decoded frame to the frame callback (or drop corrupt frames).
    bool deliverFrame(struct AVFrame* frame,
                      uint64_t timestamp,
                      int log_index,
                      bool log);

    VideoBackend video_backend_ =
#ifdef __SWITCH__
        VideoBackend::HardwareZeroCopy;
#else
        VideoBackend::Software;
#endif
    VideoCodec video_codec_ = VideoCodec::H264;
    void* codec_ctx_ = nullptr;
    void* parser_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;
    FrameCallback on_frame_;
    struct PerfStats* perf_ = nullptr;
    bool initialized_ = false;
    int error_log_count_ = 0;
};

/// Xbox / WebRTC path: H.264 only, allocation-free access-unit inspection, and
/// standalone SPS/PPS handling restored to the pre-0.2 unified behavior so an
/// encoder refresh does not desynchronize the decoder.
class XboxVideoDecoder : public VideoDecoder {
public:
    VideoAccessUnitInfo inspectAccessUnit(const uint8_t* data,
                                          size_t len) const override;
    bool initialize(int width = 1280, int height = 720) override;
    bool decode(const uint8_t* data, size_t len, uint64_t timestamp,
                const VideoAccessUnitInfo* inspected_access_unit = nullptr) override;
    bool resetForKeyframe() override;

private:
    bool decoder_ready_ = false;
    bool seen_sps_ = false;
    bool seen_pps_ = false;
    std::vector<uint8_t> parameter_sets_;
    bool parameter_sets_pending_ = false;
    int wait_log_count_ = 0;
};

/// PlayStation / Chiaki path: H.264 + HEVC, richer inspection, and standalone
/// parameter-set handling owned entirely by this class.
class PsVideoDecoder : public VideoDecoder {
public:
    VideoAccessUnitInfo inspectAccessUnit(const uint8_t* data,
                                          size_t len) const override;
    bool initialize(int width = 1280, int height = 720) override;
    bool decode(const uint8_t* data, size_t len, uint64_t timestamp,
                const VideoAccessUnitInfo* inspected_access_unit = nullptr) override;
    bool resetForKeyframe() override;

private:
    bool decoder_ready_ = false;
    bool seen_vps_ = false;
    bool seen_sps_ = false;
    bool seen_pps_ = false;
    std::vector<uint8_t> parameter_sets_;
    bool parameter_sets_pending_ = false;
    int wait_log_count_ = 0;
};

} // namespace lunar::stream
