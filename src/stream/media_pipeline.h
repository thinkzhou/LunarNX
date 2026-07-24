#pragma once

#include "../common.h"
#include "audio_packet_reorder.h"
#include "stream_backend_provider.h"
#include <atomic>
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
struct AudioFrame;
struct PerfStats;
struct VideoFrame;
class VideoDecoder;
class VideoRenderer;

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
    VideoBackend video_backend = VideoBackend::HardwareZeroCopy;
    PostProcessMode post_process_mode = PostProcessMode::Off;
    bool dithering_enabled = false;
    float dithering_strength = 3.0f;
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
    // A media discontinuity requires a fresh IDR.  The video worker performs
    // the decoder reset independently; the session acknowledges this flag
    // after a throttled control/PLI request succeeds.
    bool hasVideoRecoveryRequest() const { return video_recovery_request_.load(); }
    void clearVideoRecoveryRequest() { video_recovery_request_ = false; }
    void presentVideoFrame();

    bool isRunning() const { return running_.load(); }

private:
    struct QueuedVideoPacket {
        uint64_t timestamp = 0;
        uint32_t generation = 0;
        std::vector<uint8_t> data;
    };

    bool enqueueVideoPacket(const uint8_t* data, size_t len, uint64_t timestamp);
    bool enqueueAudioPacket(const uint8_t* data,
                            size_t len,
                            uint16_t sequence,
                            uint64_t timestamp);
    bool startWorkers(uint32_t generation);
    void stopWorkers();
    void videoWorkerLoop();
    void audioWorkerLoop();
    void processVideoPacket(const QueuedVideoPacket& packet);
    void markVideoRecovery(const char* reason);

    bool isGenerationActive(uint32_t generation) const;
    void handleVideoFrame(const VideoFrame& frame, uint32_t generation);
    void handleAudioFrame(const AudioFrame& frame, uint32_t generation);
    void shutdownUnlocked();

    StreamBackendProvider& provider_;
    std::unique_ptr<VideoDecoder> video_decoder_;
    std::unique_ptr<VideoRenderer> video_renderer_;
    std::unique_ptr<AudioDecoder> audio_decoder_;
    std::unique_ptr<AudioPlayer> audio_player_;
    std::unique_ptr<AVSync> av_sync_;

    PerfStats* perf_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> generation_{0};
    mutable std::mutex lifecycle_mutex_;

    std::mutex video_queue_mutex_;
    std::condition_variable video_queue_cv_;
    std::deque<QueuedVideoPacket> video_queue_;
    std::thread video_worker_;
    bool video_worker_stop_ = false;
    size_t queued_video_bytes_ = 0;
    uint32_t video_worker_generation_ = 0;
    std::atomic<bool> video_recovery_request_{false};
    std::atomic<bool> video_decoder_reset_pending_{false};

    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<EncodedAudioPacket> audio_queue_;
    std::thread audio_worker_;
    bool audio_worker_stop_ = false;
    size_t queued_audio_bytes_ = 0;
    uint32_t audio_worker_generation_ = 0;
    uint64_t last_decoded_audio_end_ns_ = 0;
};

} // namespace lunar::stream
