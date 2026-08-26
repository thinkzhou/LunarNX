#pragma once

#include "../common.h"
#include "../diagnostics.h"
#include "audio_packet_reorder.h"
#include "audio_decoder.h"
#include "bounded_video_queue_policy.h"
#include "realtime_latency_policy.h"
#include "stream_backend_provider.h"
#include "video_codec.h"
#include <atomic>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lunar::stream {

class AudioDecoder;
class AudioPlayer;
class AVSync;
struct PerfStats;
struct VideoFrame;
class VideoDecoder;
class VideoRenderer;

enum class VideoRenderStage : uint8_t {
    Idle = 0,
    WaitingGpuMutex,
    WaitingRenderMutex,
    DecoderResetFence,
    ResolutionTransitionFence,
    MappingUpdate,
    FramebufferAcquire,
    PresentFence,
    RecordCommands,
    SubmitCommands,
    Fault,
    Shutdown,
};

inline const char* videoRenderStageName(VideoRenderStage stage) {
    switch (stage) {
        case VideoRenderStage::Idle: return "idle";
        case VideoRenderStage::WaitingGpuMutex: return "waiting-gpu-mutex";
        case VideoRenderStage::WaitingRenderMutex: return "waiting-render-mutex";
        case VideoRenderStage::DecoderResetFence: return "decoder-reset-fence";
        case VideoRenderStage::ResolutionTransitionFence:
            return "resolution-transition-fence";
        case VideoRenderStage::MappingUpdate: return "mapping-update";
        case VideoRenderStage::FramebufferAcquire:
            return "framebuffer-acquire";
        case VideoRenderStage::PresentFence: return "present-fence";
        case VideoRenderStage::RecordCommands: return "record-commands";
        case VideoRenderStage::SubmitCommands: return "submit-commands";
        case VideoRenderStage::Fault: return "fault";
        case VideoRenderStage::Shutdown: return "shutdown";
    }
    return "unknown";
}

struct MediaHealthStats {
    bool has_decoded_video = false;
    bool has_presented_video = false;
    uint64_t decoded_video_age_ms = 0;
    uint64_t presented_video_age_ms = 0;
    uint32_t render_fault_count = 0;
    uint32_t consecutive_render_faults = 0;
    VideoRenderStage renderer_stage = VideoRenderStage::Idle;
    uint64_t renderer_stage_age_ms = 0;
};

enum class PostProcessMode {
    Off,
    Upscale,
    UpscaleRcas,
};

enum class VideoBackend {
    HardwareZeroCopy,
    HardwareCopyOut,
    Software,
};

// Selects where complete encoded access units are decoded. The transport
// remains responsible for choosing a policy; decoder and renderer components
// stay protocol-neutral.
enum class VideoSchedulingMode {
    RealtimeQueued,
    DirectLowLatency,
    BoundedLowLatency,
};

struct VideoQueueLimits {
    size_t max_packets = 3;
    size_t max_bytes = 8 * 1024 * 1024;
    std::chrono::milliseconds max_age{50};
};

inline const char* videoBackendName(VideoBackend backend) {
    switch (backend) {
        case VideoBackend::HardwareZeroCopy: return "hardware_zero_copy";
        case VideoBackend::HardwareCopyOut: return "hardware_copy_out";
        case VideoBackend::Software: return "software";
    }
    return "unknown";
}

inline const char* videoBackendOverlayName(VideoBackend backend) {
    switch (backend) {
        case VideoBackend::HardwareZeroCopy: return "HW-ZC";
        case VideoBackend::HardwareCopyOut: return "HW-Copy";
        case VideoBackend::Software: return "SW";
    }
    return "Unknown";
}

inline bool usesHardwareDecode(VideoBackend backend) {
    return backend == VideoBackend::HardwareZeroCopy ||
           backend == VideoBackend::HardwareCopyOut;
}

inline bool usesZeroCopyRender(VideoBackend backend) {
    return backend == VideoBackend::HardwareZeroCopy;
}

struct MediaPipelineOptions {
    VideoPipelinePath video_path = VideoPipelinePath::Xbox;
    VideoCodec video_codec = VideoCodec::H264;
    VideoBackend video_backend = VideoBackend::HardwareZeroCopy;
    PostProcessMode post_process_mode = PostProcessMode::Off;
    bool dithering_enabled = false;
    float dithering_strength = 3.0f;
    bool hold_non_target_startup_frames = false;
    VideoSchedulingMode video_scheduling = VideoSchedulingMode::RealtimeQueued;
    VideoQueueLimits video_queue_limits{};
    VideoPresentationMode video_presentation_mode =
        VideoPresentationMode::BufferedFifo;
    VideoDecodeCatchUpMode video_decode_catch_up_mode =
        VideoDecodeCatchUpMode::Disabled;
    AudioLatencyMode audio_latency_mode = AudioLatencyMode::Resilient;
};

/// Owns the media half of a streaming session.
///
/// StreamController remains responsible for Xbox session/WebRTC/input. This
/// pipeline owns decode, AV sync, render scheduling, and audio output, matching
/// Moonlight-Switch's session/provider/renderer separation at LunarNX's scale.
class MediaPipeline {
public:
    explicit MediaPipeline(StreamBackendProvider& provider);
    ~MediaPipeline();

    bool initialize(int width, int height, PerfStats* perf,
                    const MediaPipelineOptions& options = {});
    void shutdown();

    bool decodeVideoPacket(const uint8_t* data, size_t len, uint64_t timestamp);
    bool decodeAudioPacket(const uint8_t* data,
                           size_t len,
                           uint16_t sequence,
                           uint64_t timestamp);
    // Queue already-decoded PCM audio (Chiaki path). Audren is only touched by
    // the media audio worker, just like the Xbox Opus decode path.
    bool playDecodedAudio(const AudioFrame& frame);
    // Transport-specific ingress accounting. PS chiaki supplies complete
    // access units rather than the RTP packets used by Xbox/WebRTC.
    void recordIncomingVideoSample(size_t bytes, uint64_t pts_ns, uint32_t frames_lost);
    void recordIncomingAudioPacket();
    void setVideoReadyCallback(std::function<void()> callback);
    // A media discontinuity requires a fresh IDR.  The video worker performs
    // the decoder reset independently; the session acknowledges this flag
    // after a throttled control/PLI request succeeds.
    bool hasVideoRecoveryRequest() const { return video_recovery_request_.load(); }
    bool hasRendererRecoveryPending() const {
        return video_renderer_recovery_pending_.load(std::memory_order_acquire);
    }
    void clearVideoRecoveryRequest();
    // Ask the transport for an IDR. Packet-loss recovery keeps the active
    // decoder; only invalid H.264 or a bounded-buffer failure requests a
    // decoder reset on the video worker.
    void requestVideoRecovery(const char* reason, bool reset_decoder = false);
    // Presentation-only stalls keep the decoder and its reference chain alive.
    // The video worker asks the UI thread to retire GPU command-ring slices,
    // without flushing FFmpeg or waiting from the WebRTC owner loop.
    void requestRendererRecovery(const char* reason);
    // Treat a new WebRTC/RTP association as a new encoded video source. This
    // drains GPU-owned frames, flushes decoder/parser state, and re-anchors
    // AV sync before accepting the next IDR.
    void prepareForNewVideoSource(const char* reason);
    // Treat a new WebRTC association as a complete media source change. This
    // resets both video and audio RTP/decode/playback state.
    void prepareForNewMediaSource(const char* reason);
    void presentVideoFrame();
    void setVideoPresentationMode(VideoPresentationMode mode);
    void setVideoDecodeCatchUpMode(VideoDecodeCatchUpMode mode);
    bool setAudioLatencyMode(AudioLatencyMode mode);
    MediaHealthStats getHealthStats() const;

    bool isRunning() const { return running_.load(); }

private:
    struct QueuedVideoPacket {
        uint64_t timestamp = 0;
        uint32_t generation = 0;
        uint32_t recovery_epoch = 0;
        bool contains_idr = false;
        bool contains_vcl = false;
        VideoAccessUnitInfo access_unit;
        std::chrono::steady_clock::time_point enqueued_at;
        uint64_t queue_age_us = 0;
        bool suppress_output = false;
        std::vector<uint8_t> data;
    };

    struct QueuedDecodedAudio {
        AudioFrame frame;
        uint32_t generation = 0;
        uint32_t source_epoch = 0;
    };

    struct BoundedVideoStats {
        uint64_t enqueued = 0;
        uint64_t decoded = 0;
        uint64_t intentional_drop = 0;
        uint64_t alloc_fail = 0;
        size_t depth_high = 0;
        size_t bytes_high = 0;
        uint64_t oldest_age_max_us = 0;
        uint64_t enqueue_copy_total_us = 0;
        uint64_t enqueue_copy_max_us = 0;
        uint64_t worker_decode_total_us = 0;
        uint64_t worker_decode_max_us = 0;
        uint64_t recovery_overflow = 0;
        uint64_t recovery_age = 0;
        uint64_t recovery_decode = 0;
        uint64_t idr_requests = 0;
        std::chrono::steady_clock::time_point window_start{};
    };

    bool enqueueVideoPacket(const uint8_t* data, size_t len, uint64_t timestamp);
    bool decodeVideoDirect(const uint8_t* data, size_t len, uint64_t timestamp);
    bool enqueueAudioPacket(const uint8_t* data,
                            size_t len,
                            uint16_t sequence,
                            uint64_t timestamp);
    bool startWorkers(uint32_t generation);
    void stopWorkers();
    void videoWorkerLoop();
    void audioWorkerLoop();
    void processVideoPacket(const QueuedVideoPacket& packet);
    bool resetVideoDecoderForKeyframe(bool new_source = false);
    void beginHardVideoRecovery(const char* reason,
                                bool force_new_epoch = false);
    bool beginHardVideoRecoveryLocked(bool force_new_epoch,
                                      size_t& dropped_packets,
                                      size_t& dropped_bytes);
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    void recordVideoQueueLocked();
#endif
    void maybeLogBoundedVideoStatsLocked(
        std::chrono::steady_clock::time_point now);

    bool isGenerationActive(uint32_t generation) const;
    void handleVideoFrame(const VideoFrame& frame, uint32_t generation);
    void handleAudioFrame(const AudioFrame& frame,
                          uint32_t generation,
                          uint32_t source_epoch);
    bool submitDecodedAudio(const AudioFrame& frame,
                            uint32_t generation,
                            uint32_t source_epoch);
    void shutdownUnlocked();
    PerfStats* perfStats() const {
        return perf_.load(std::memory_order_relaxed);
    }

    StreamBackendProvider& provider_;
    std::unique_ptr<VideoDecoder> video_decoder_;
    std::unique_ptr<VideoRenderer> video_renderer_;
    std::unique_ptr<AudioDecoder> audio_decoder_;
    std::unique_ptr<AudioPlayer> audio_player_;
    std::unique_ptr<AVSync> av_sync_;

    std::atomic<PerfStats*> perf_{nullptr};
    std::atomic<VideoSchedulingMode> video_scheduling_{
        VideoSchedulingMode::RealtimeQueued};
    std::atomic<VideoDecodeCatchUpMode> video_decode_catch_up_mode_{
        VideoDecodeCatchUpMode::Disabled};
    // NVDEC may return a frame submitted by an earlier decode() call. Match
    // catch-up suppression by RTP timestamp instead of the current callback
    // stack so decoder reordering cannot hide the wrong frame.
    std::mutex video_suppression_mutex_;
    std::deque<uint64_t> suppressed_video_timestamps_;
    VideoQueueLimits video_queue_limits_{};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> generation_{0};
    // DirectLowLatency decoding invokes the frame callback synchronously. A
    // recursive mutex lets that callback enter handleVideoFrame while keeping
    // shutdown from destroying decoder/renderer state underneath the callback.
    mutable std::recursive_mutex lifecycle_mutex_;
    std::mutex video_ready_callback_mutex_;
    std::function<void()> video_ready_callback_;
    std::atomic<bool> video_ready_notified_{false};

    std::mutex video_queue_mutex_;
    std::condition_variable video_queue_cv_;
    std::deque<QueuedVideoPacket> video_queue_;
    std::thread video_worker_;
    bool video_worker_stop_ = false;
    size_t queued_video_bytes_ = 0;
    uint32_t video_worker_generation_ = 0;
    std::atomic<bool> video_recovery_request_{false};
    std::atomic<bool> video_decoder_reset_pending_{false};
    std::atomic<bool> video_renderer_recovery_pending_{false};
    std::atomic<bool> video_renderer_recovery_in_progress_{false};
    std::atomic<bool> video_new_source_pending_{false};
    std::atomic<bool> video_waiting_for_keyframe_{false};
    std::atomic<uint32_t> video_recovery_epoch_{0};
    bool video_decoder_reset_wakeup_ = false;
    bool video_renderer_recovery_wakeup_ = false;
    BoundedVideoStats bounded_video_stats_{};

    std::atomic<uint64_t> last_decoded_video_ns_{0};
    std::atomic<uint32_t> decoded_video_frames_{0};
    std::atomic<uint32_t> render_fault_count_{0};
    // Published by the UI/present thread so the WebRTC owner loop can inspect
    // liveness without taking the renderer/lifecycle mutex.
    std::atomic<uint64_t> last_presented_video_ns_{0};
    std::atomic<uint32_t> consecutive_render_faults_{0};
    // VideoRenderer writes these without logging on the hot path. The session
    // watchdog can therefore report the last Deko3D boundary even if the UI
    // thread is blocked inside a lock or driver call.
    std::atomic<uint8_t> renderer_stage_{
        static_cast<uint8_t>(VideoRenderStage::Idle)};
    std::atomic<uint64_t> renderer_stage_started_ns_{0};

    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<EncodedAudioPacket> audio_queue_;
    std::deque<QueuedDecodedAudio> decoded_audio_queue_;
    std::thread audio_worker_;
    bool audio_worker_stop_ = false;
    size_t queued_audio_bytes_ = 0;
    size_t queued_decoded_audio_bytes_ = 0;
    uint32_t audio_worker_generation_ = 0;
    std::atomic<uint64_t> last_decoded_audio_end_ns_{0};
    std::atomic<uint32_t> audio_source_epoch_{0};
    std::atomic<bool> audio_source_reset_pending_{false};
    std::atomic<uint32_t> audio_decode_epoch_{0};
};

} // namespace lunar::stream
