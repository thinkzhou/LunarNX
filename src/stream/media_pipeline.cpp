#include "media_pipeline.h"
#include "../diagnostics.h"
#include "audio_decoder.h"
#include "audio_timing.h"
#include "audio_player.h"
#include "av_sync.h"
#include "perf_stats.h"
#include "video_decoder.h"
#include "video_renderer.h"
#include "video_sync_policy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>

#ifdef __SWITCH__
#include <borealis.hpp>
#endif

namespace lunar::stream {

namespace {
constexpr size_t kMaxVideoQueuePackets = 2048;
constexpr size_t kMaxVideoQueueBytes = 32 * 1024 * 1024;
constexpr size_t kMaxAudioQueuePackets = 512;
constexpr size_t kMaxAudioQueueBytes = 4 * 1024 * 1024;
constexpr int kMediaQueueLogLimit = 32;
constexpr int kMediaWorkerLogLimit = 32;
constexpr int kVideoSyncLogLimit = 16;

std::atomic<int> g_media_queue_logs{0};
std::atomic<int> g_media_worker_logs{0};
std::atomic<int> g_video_sync_logs{0};

bool shouldLogMediaQueue() {
    return g_media_queue_logs.fetch_add(1) < kMediaQueueLogLimit;
}

bool shouldLogMediaWorker() {
    return g_media_worker_logs.fetch_add(1) < kMediaWorkerLogLimit;
}
}

MediaPipeline::MediaPipeline(StreamBackendProvider& provider)
    : provider_(provider) {}

MediaPipeline::~MediaPipeline() {
    shutdown();
}

bool MediaPipeline::initialize(int width, int height, PerfStats* perf,
                               const MediaPipelineOptions& options) {
    running_ = false;
    video_recovery_request_ = false;
    video_decoder_reset_pending_ = false;
    stopWorkers();

    uint32_t worker_generation = 0;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        lunar::diagnosticLog("media", "initialize begin width=%d height=%d",
                             width,
                             height);
        shutdownUnlocked();

        const uint32_t generation = generation_.fetch_add(1) + 1;
        perf_ = perf;

        try {
            lunar::diagnosticLog("media", "create components begin backend=%s",
                                 videoBackendName(options.video_backend));
            lunar::diagnosticLog("media", "create video decoder begin");
            video_decoder_ = provider_.createVideoDecoder();
            lunar::diagnosticLog("media", "create video decoder done ptr=%p",
                                 video_decoder_.get());
            lunar::diagnosticLog("media", "create video renderer begin");
            video_renderer_ = provider_.createVideoRenderer();
            lunar::diagnosticLog("media", "create video renderer done ptr=%p",
                                 video_renderer_.get());
            lunar::diagnosticLog("media", "create audio decoder begin");
            audio_decoder_ = provider_.createAudioDecoder();
            lunar::diagnosticLog("media", "create audio decoder done ptr=%p",
                                 audio_decoder_.get());
            lunar::diagnosticLog("media", "create audio player begin");
            audio_player_ = provider_.createAudioPlayer();
            lunar::diagnosticLog("media", "create audio player done ptr=%p",
                                 audio_player_.get());
            lunar::diagnosticLog("media", "create av sync begin");
            av_sync_ = provider_.createAVSync();
            lunar::diagnosticLog("media", "create av sync done ptr=%p",
                                 av_sync_.get());
            lunar::diagnosticLog("media", "create components done");
        } catch (const std::exception& e) {
            lunar::diagnosticLog("media", "create components exception: %s",
                                 e.what());
            shutdownUnlocked();
            return false;
        } catch (...) {
            lunar::diagnosticLog("media", "create components unknown exception");
            shutdownUnlocked();
            return false;
        }

        if (!video_decoder_ || !video_renderer_ || !audio_decoder_ ||
            !audio_player_ || !av_sync_) {
            fprintf(stderr, "[media] backend provider returned null component\n");
            lunar::diagnosticLog("media", "backend provider returned null component");
            shutdownUnlocked();
            return false;
        }

        video_decoder_->setVideoBackend(options.video_backend);
        video_decoder_->setPerfStats(perf_);
        lunar::diagnosticLog("media", "video decoder init begin");
        if (!video_decoder_->initialize(width, height)) {
            lunar::diagnosticLog("media", "video decoder init failed");
            shutdownUnlocked();
            return false;
        }
        lunar::diagnosticLog("media", "video decoder init done");
        video_decoder_->setCallback([this, generation](const VideoFrame& frame) {
            handleVideoFrame(frame, generation);
        });
        lunar::diagnosticLog("media", "video decoder callback set generation=%u",
                             generation);

        audio_decoder_->setPerfStats(perf_);
        lunar::diagnosticLog("media", "audio decoder init begin");
        if (!audio_decoder_->initialize()) {
            lunar::diagnosticLog("media", "audio decoder init failed");
            shutdownUnlocked();
            return false;
        }
        lunar::diagnosticLog("media", "audio decoder init done");
        audio_decoder_->setCallback([this, generation](const AudioFrame& frame) {
            handleAudioFrame(frame, generation);
        });
        lunar::diagnosticLog("media", "audio decoder callback set generation=%u",
                             generation);

        audio_player_->setPerfStats(perf_);
        lunar::diagnosticLog("media", "audio player init begin");
        if (!audio_player_->initialize(48000, 2)) {
            lunar::diagnosticLog("media", "audio player init failed");
            shutdownUnlocked();
            return false;
        }
        lunar::diagnosticLog("media", "audio player init done");

        const auto renderer_backend = usesZeroCopyRender(options.video_backend)
            ? VideoBackend::HardwareZeroCopy
            : VideoBackend::Software;
        lunar::diagnosticLog("media",
                             "video renderer backend=%s decode_backend=%s",
                             videoBackendName(renderer_backend),
                             videoBackendName(options.video_backend));
        video_renderer_->setVideoBackend(renderer_backend);
        video_renderer_->setPerfStats(perf_);
        video_renderer_->setPostProcessMode(options.post_process_mode);
        video_renderer_->setDitheringEnabled(options.dithering_enabled,
                                             options.dithering_strength);
        lunar::diagnosticLog("media", "video renderer init begin");
        if (!video_renderer_->initialize("LunarNX", width, height)) {
            lunar::diagnosticLog("media", "video renderer init failed");
            shutdownUnlocked();
            return false;
        }
        lunar::diagnosticLog("media", "video renderer init done");

        lunar::diagnosticLog("media", "av sync start begin");
        av_sync_->start();
        lunar::diagnosticLog("media", "av sync start done");
        running_ = true;
        worker_generation = generation;
        lunar::diagnosticLog("media", "initialize done generation=%u", generation);
    }

    if (startWorkers(worker_generation)) return true;

    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    generation_.fetch_add(1);
    shutdownUnlocked();
    return false;
}

void MediaPipeline::shutdown() {
    lunar::diagnosticLog("media", "shutdown begin");
    running_ = false;
    stopWorkers();
    lunar::diagnosticLog("media", "shutdown after worker stop");
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    generation_.fetch_add(1);
    shutdownUnlocked();
    lunar::diagnosticLog("media", "shutdown done");
}

void MediaPipeline::shutdownUnlocked() {
    running_ = false;
    video_recovery_request_ = false;
    video_decoder_reset_pending_ = false;
    if (video_decoder_) {
        lunar::diagnosticLog("media", "shutdown video decoder begin");
        video_decoder_->shutdown();
        lunar::diagnosticLog("media", "shutdown video decoder done");
    }
    if (audio_decoder_) {
        lunar::diagnosticLog("media", "shutdown audio decoder begin");
        audio_decoder_->shutdown();
        lunar::diagnosticLog("media", "shutdown audio decoder done");
    }
    if (video_renderer_) {
        lunar::diagnosticLog("media", "shutdown video renderer begin");
        video_renderer_->shutdown();
        lunar::diagnosticLog("media", "shutdown video renderer done");
    }
    if (audio_player_) {
        lunar::diagnosticLog("media", "shutdown audio player begin");
        audio_player_->shutdown();
        lunar::diagnosticLog("media", "shutdown audio player done");
    }
    if (av_sync_) {
        lunar::diagnosticLog("media", "shutdown av sync begin");
        av_sync_->reset();
        lunar::diagnosticLog("media", "shutdown av sync done");
    }

    lunar::diagnosticLog("media", "shutdown reset components begin");
    video_decoder_.reset();
    video_renderer_.reset();
    audio_decoder_.reset();
    audio_player_.reset();
    av_sync_.reset();
    perf_ = nullptr;
    lunar::diagnosticLog("media", "shutdown reset components done");
}

bool MediaPipeline::decodeVideoPacket(const uint8_t* data, size_t len,
                                      uint64_t timestamp) {
    return enqueueVideoPacket(data, len, timestamp);
}

bool MediaPipeline::decodeAudioPacket(const uint8_t* data, size_t len,
                                      uint16_t sequence,
                                      uint64_t timestamp) {
    return enqueueAudioPacket(data, len, sequence, timestamp);
}

void MediaPipeline::markVideoRecovery(const char* reason) {
    video_decoder_reset_pending_ = true;
    const bool was_pending = video_recovery_request_.exchange(true);
    if (!was_pending) {
        lunar::diagnosticLog("media", "video recovery requested reason=%s",
                             reason ? reason : "unknown");
    }
}

bool MediaPipeline::enqueueVideoPacket(const uint8_t* data,
                                       size_t len,
                                       uint64_t timestamp) {
    if (!running_ || !data || len == 0 || len > kMaxVideoQueueBytes) return false;

    QueuedVideoPacket packet;
    packet.timestamp = timestamp;
    packet.generation = generation_.load();
    try {
        packet.data.assign(data, data + len);
    } catch (...) {
        lunar::diagnosticLog("media", "video queue alloc failed len=%zu", len);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        if (video_worker_stop_ || !running_) return false;
        if (!video_queue_.empty() &&
            (video_queue_.size() >= kMaxVideoQueuePackets ||
             queued_video_bytes_ + packet.data.size() > kMaxVideoQueueBytes)) {
            const size_t dropped_packets = video_queue_.size();
            video_queue_.clear();
            queued_video_bytes_ = 0;
            markVideoRecovery("video queue overflow");
            for (size_t i = 0; i < dropped_packets; ++i) {
                if (perf_) perf_->recordVideoFrameDrop();
            }
            if (shouldLogMediaQueue()) {
                lunar::diagnosticLog("media",
                                     "video queue reset dropped=%zu packets=%zu bytes=%zu",
                                     dropped_packets,
                                     video_queue_.size(),
                                     queued_video_bytes_);
            }
        }
        video_queue_.push_back(std::move(packet));
        queued_video_bytes_ += video_queue_.back().data.size();
        if (shouldLogMediaQueue()) {
            lunar::diagnosticLog("media",
                                 "video queue push len=%zu packets=%zu bytes=%zu",
                                 len,
                                 video_queue_.size(),
                                 queued_video_bytes_);
        }
    }
    video_queue_cv_.notify_one();
    return true;
}

bool MediaPipeline::enqueueAudioPacket(const uint8_t* data,
                                       size_t len,
                                       uint16_t sequence,
                                       uint64_t timestamp) {
    if (!running_ || !data || len == 0) return false;
    if (len > kMaxAudioQueueBytes) {
        lunar::diagnosticLog("media", "audio packet too large len=%zu", len);
        if (perf_) perf_->recordAudioDrop();
        return false;
    }

    EncodedAudioPacket packet;
    packet.sequence = sequence;
    packet.timestamp = timestamp;
    packet.generation = generation_.load();
    try {
        packet.data.assign(data, data + len);
    } catch (...) {
        lunar::diagnosticLog("media", "audio queue alloc failed len=%zu", len);
        if (perf_) perf_->recordAudioDrop();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (audio_worker_stop_ || !running_) return false;
        while (!audio_queue_.empty() &&
               (audio_queue_.size() >= kMaxAudioQueuePackets ||
                queued_audio_bytes_ + packet.data.size() > kMaxAudioQueueBytes)) {
            queued_audio_bytes_ -= audio_queue_.front().data.size();
            audio_queue_.pop_front();
            if (perf_) perf_->recordAudioDrop();
            if (shouldLogMediaQueue()) {
                lunar::diagnosticLog("media",
                                     "audio queue drop packets=%zu bytes=%zu",
                                     audio_queue_.size(),
                                     queued_audio_bytes_);
            }
        }
        audio_queue_.push_back(std::move(packet));
        queued_audio_bytes_ += audio_queue_.back().data.size();
    }
    audio_queue_cv_.notify_one();
    return true;
}

bool MediaPipeline::startWorkers(uint32_t generation) {
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        video_worker_stop_ = false;
        video_worker_generation_ = generation;
        video_queue_.clear();
        queued_video_bytes_ = 0;
        video_recovery_request_ = false;
        video_decoder_reset_pending_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_worker_stop_ = false;
        audio_worker_generation_ = generation;
        audio_queue_.clear();
        queued_audio_bytes_ = 0;
        last_decoded_audio_end_ns_ = 0;
    }
    g_media_queue_logs = 0;
    g_media_worker_logs = 0;
    g_video_sync_logs = 0;

    try {
        video_worker_ = std::thread(&MediaPipeline::videoWorkerLoop, this);
        audio_worker_ = std::thread(&MediaPipeline::audioWorkerLoop, this);
    } catch (const std::exception& e) {
        lunar::diagnosticLog("media", "worker create failed: %s", e.what());
        stopWorkers();
        return false;
    } catch (...) {
        lunar::diagnosticLog("media", "worker create failed");
        stopWorkers();
        return false;
    }
    lunar::diagnosticLog("media", "workers started generation=%u", generation);
    return true;
}

void MediaPipeline::stopWorkers() {
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        video_worker_stop_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_worker_stop_ = true;
    }
    video_queue_cv_.notify_all();
    audio_queue_cv_.notify_all();

    if (video_worker_.joinable()) {
        video_worker_.join();
        lunar::diagnosticLog("media", "video worker joined");
    }
    if (audio_worker_.joinable()) {
        audio_worker_.join();
        lunar::diagnosticLog("media", "audio worker joined");
    }

    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        video_queue_.clear();
        queued_video_bytes_ = 0;
        video_worker_generation_ = 0;
        video_worker_stop_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_queue_.clear();
        queued_audio_bytes_ = 0;
        audio_worker_generation_ = 0;
        audio_worker_stop_ = false;
        last_decoded_audio_end_ns_ = 0;
    }
}

void MediaPipeline::videoWorkerLoop() {
    lunar::diagnosticLog("media", "video worker loop begin generation=%u",
                         video_worker_generation_);
    while (true) {
        QueuedVideoPacket packet;
        {
            std::unique_lock<std::mutex> lock(video_queue_mutex_);
            video_queue_cv_.wait(lock, [this]() {
                return video_worker_stop_ || !video_queue_.empty();
            });
            if (video_worker_stop_) break;
            packet = std::move(video_queue_.front());
            video_queue_.pop_front();
            queued_video_bytes_ -= packet.data.size();
        }
        if (video_decoder_reset_pending_.exchange(false) && video_decoder_) {
            if (!video_decoder_->resetForKeyframe()) {
                lunar::diagnosticLog("media", "video decoder reset for keyframe failed");
            }
        }
        if (shouldLogMediaWorker()) {
            lunar::diagnosticLog("media", "video worker pop len=%zu", packet.data.size());
        }
        processVideoPacket(packet);
    }
    lunar::diagnosticLog("media", "video worker loop end");
}

void MediaPipeline::audioWorkerLoop() {
    lunar::diagnosticLog("media", "audio worker loop begin generation=%u",
                         audio_worker_generation_);
    AudioPacketReorder reorder;
    while (true) {
        EncodedAudioPacket packet;
        {
            std::unique_lock<std::mutex> lock(audio_queue_mutex_);
            audio_queue_cv_.wait(lock, [this]() {
                return audio_worker_stop_ || !audio_queue_.empty();
            });
            if (audio_worker_stop_) break;
            packet = std::move(audio_queue_.front());
            audio_queue_.pop_front();
            queued_audio_bytes_ -= packet.data.size();
        }

        if (packet.generation != audio_worker_generation_ ||
            !isGenerationActive(packet.generation)) {
            continue;
        }
        for (auto& action : reorder.push(std::move(packet))) {
            if (!audio_decoder_ || !isGenerationActive(audio_worker_generation_)) break;
            if (action.type == AudioReorderAction::Type::Missing) {
                if (last_decoded_audio_end_ns_ == 0) {
                    if (perf_) perf_->recordAudioDrop();
                    continue;
                }
                audio_decoder_->decodeMissing(last_decoded_audio_end_ns_);
            } else {
                audio_decoder_->decode(action.packet.data.data(),
                                       action.packet.data.size(),
                                       action.packet.timestamp);
            }
        }
    }
    lunar::diagnosticLog("media", "audio worker loop end");
}

void MediaPipeline::processVideoPacket(const QueuedVideoPacket& packet) {
    if (!running_ || packet.data.empty() ||
        packet.generation != video_worker_generation_ ||
        !isGenerationActive(packet.generation)) {
        return;
    }
    if (video_decoder_) {
        const bool decoded = video_decoder_->decode(packet.data.data(),
                                                    packet.data.size(),
                                                    packet.timestamp);
        if (!decoded) {
            markVideoRecovery("video decoder error");
            // Keep the reset on the video worker so FFmpeg/NVDEC is never
            // touched concurrently by the WebRTC pump or UI thread.
            video_decoder_->resetForKeyframe();
            video_decoder_reset_pending_ = false;
        }
    }
}

bool MediaPipeline::isGenerationActive(uint32_t generation) const {
    return running_.load() && generation_.load() == generation;
}

void MediaPipeline::handleVideoFrame(const VideoFrame& frame,
                                     uint32_t generation) {
    int64_t delay_ns = 0;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!isGenerationActive(generation) || !av_sync_ || !video_renderer_) return;

        av_sync_->updateVideoPts(frame.timestamp);
        delay_ns = av_sync_->getVideoDelayNs(frame.timestamp);
    }

    const auto action = videoSyncAction(delay_ns);
    if (action == VideoSyncAction::Drop) {
        if (perf_) perf_->recordVideoFrameDrop();
        if (g_video_sync_logs.fetch_add(1) < kVideoSyncLogLimit) {
            lunar::diagnosticLog("media", "video sync drop lag_ns=%lld",
                                 static_cast<long long>(delay_ns));
        }
        return;
    }
    if (action == VideoSyncAction::Wait) {
        const int64_t wait_ns = videoSyncWaitNs(delay_ns);
        if (g_video_sync_logs.fetch_add(1) < kVideoSyncLogLimit) {
            lunar::diagnosticLog("media", "video sync wait lead_ns=%lld wait_ns=%lld",
                                 static_cast<long long>(delay_ns),
                                 static_cast<long long>(wait_ns));
        }
        // Do not sleep the decode worker for a frame that is early.  At
        // 1080p/HQ that stall can fill the encoded queue and lose an entire
        // reference chain.  The renderer's latest-frame handoff and the
        // display pacing provide the appropriate wait point instead.
        (void)wait_ns;
    }

    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!isGenerationActive(generation) || !video_renderer_) return;
        const bool rendered = video_renderer_->render(frame);
        if (rendered && perf_) perf_->recordFrame();
    }
}

void MediaPipeline::handleAudioFrame(const AudioFrame& frame,
                                     uint32_t generation) {
    if (!isGenerationActive(generation)) return;

    last_decoded_audio_end_ns_ =
        frame.timestamp + audioSamplesToNanoseconds(frame.sample_count,
                                                    frame.sample_rate);
    if (!audio_player_ || !av_sync_) return;
    if (!audio_player_->play(frame)) return;

    const uint64_t playback_timestamp = estimateAudioPlaybackTimestamp(
        frame.timestamp,
        frame.sample_count,
        frame.sample_rate,
        audio_player_->queuedSampleCount());
    av_sync_->updateAudioPts(playback_timestamp);
}

void MediaPipeline::presentVideoFrame() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_.load() && video_renderer_) {
        video_renderer_->present();
    }
}

} // namespace lunar::stream
