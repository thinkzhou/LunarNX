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
constexpr int kMediaQueueLogLimit = 8;
constexpr int kMediaWorkerLogLimit = 8;
constexpr int kVideoSyncLogLimit = 16;

std::atomic<int> g_media_queue_logs{0};
std::atomic<int> g_media_worker_logs{0};
std::atomic<int> g_video_sync_logs{0};
std::atomic<uint64_t> g_cloud_1080_render_frames{0};

uint64_t monotonicNowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

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
    video_renderer_recovery_pending_ = false;
    video_renderer_recovery_in_progress_ = false;
    video_new_source_pending_ = false;
    last_decoded_video_ns_ = 0;
    decoded_video_frames_ = 0;
    render_fault_count_ = 0;
    last_presented_video_ns_ = 0;
    consecutive_render_faults_ = 0;
    renderer_stage_ = static_cast<uint8_t>(VideoRenderStage::Idle);
    renderer_stage_started_ns_ = 0;
    stopWorkers();

    uint32_t worker_generation = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
        lunar::diagnosticLog("media", "initialize begin width=%d height=%d codec=%s",
                             width,
                             height,
                             videoCodecName(options.video_codec));
        shutdownUnlocked();

        const uint32_t generation = generation_.fetch_add(1) + 1;
        video_ready_notified_ = false;
        audio_latency_mode_.store(options.audio_latency_mode,
                                  std::memory_order_release);
        audio_start_gate_open_.store(
            options.video_path != VideoPipelinePath::Xbox,
            std::memory_order_release);
        audio_start_primed_.store(
            options.video_path != VideoPipelinePath::Xbox,
            std::memory_order_release);
        audio_startup_packets_skipped_.store(0, std::memory_order_release);
        perf_.store(perf, std::memory_order_release);
        video_scheduling_.store(options.video_scheduling,
                                std::memory_order_release);
        video_decode_catch_up_mode_.store(
            options.video_decode_catch_up_mode,
            std::memory_order_release);
        {
            std::lock_guard<std::mutex> suppression_lock(
                video_suppression_mutex_);
            suppressed_video_timestamps_.clear();
        }
        video_queue_limits_ = options.video_queue_limits;

        try {
            lunar::diagnosticLog("media", "create components begin backend=%s",
                                 videoBackendName(options.video_backend));
            lunar::diagnosticLog("media", "create video decoder begin");
            video_decoder_ = provider_.createVideoDecoder(options.video_path);
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
        video_decoder_->setVideoCodec(options.video_codec);
        video_decoder_->setPerfStats(perf);
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

        audio_decoder_->setPerfStats(perf);
        lunar::diagnosticLog("media", "audio decoder init begin");
        if (!audio_decoder_->initialize()) {
            lunar::diagnosticLog("media", "audio decoder init failed");
            shutdownUnlocked();
            return false;
        }
        lunar::diagnosticLog("media", "audio decoder init done");
        audio_decoder_->setCallback([this, generation](const AudioFrame& frame) {
            handleAudioFrame(frame, generation,
                             audio_decode_epoch_.load(std::memory_order_acquire));
        });
        lunar::diagnosticLog("media", "audio decoder callback set generation=%u",
                             generation);

        audio_player_->setPerfStats(perf);
        lunar::diagnosticLog("media", "audio player init begin");
        if (!audio_player_->initialize(48000, 2,
                                       options.audio_latency_mode)) {
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
        video_renderer_->setPerfStats(perf);
        video_renderer_->setPresentationMode(
            options.video_presentation_mode);
        video_renderer_->setProgressSink(&renderer_stage_,
                                         &renderer_stage_started_ns_);
        video_renderer_->setPostProcessMode(options.post_process_mode);
        video_renderer_->setDitheringEnabled(options.dithering_enabled,
                                             options.dithering_strength);
        video_renderer_->setHoldNonTargetStartupFrames(
            options.hold_non_target_startup_frames);
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

    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    generation_.fetch_add(1);
    shutdownUnlocked();
    return false;
}

void MediaPipeline::setVideoPresentationMode(VideoPresentationMode mode) {
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    if (video_renderer_) video_renderer_->setPresentationMode(mode);
}

void MediaPipeline::setVideoDecodeCatchUpMode(VideoDecodeCatchUpMode mode) {
    video_decode_catch_up_mode_.store(mode, std::memory_order_release);
}

bool MediaPipeline::setAudioLatencyMode(AudioLatencyMode mode) {
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    if (!audio_player_ || !audio_player_->setLatencyMode(mode)) return false;
    audio_latency_mode_.store(mode, std::memory_order_release);
    return true;
}

void MediaPipeline::shutdown() {
    lunar::diagnosticLog("media", "shutdown begin");
    running_ = false;
    stopWorkers();
    lunar::diagnosticLog("media", "shutdown after worker stop");
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    generation_.fetch_add(1);
    shutdownUnlocked();
    lunar::diagnosticLog("media", "shutdown done");
}

void MediaPipeline::shutdownUnlocked() {
    running_ = false;
    video_recovery_request_ = false;
    video_decoder_reset_pending_ = false;
    video_renderer_recovery_pending_ = false;
    video_renderer_recovery_in_progress_ = false;
    video_new_source_pending_ = false;
    video_waiting_for_keyframe_ = false;
    video_recovery_epoch_ = 0;
    video_ready_notified_ = false;
    audio_start_gate_open_.store(true, std::memory_order_release);
    audio_start_primed_.store(true, std::memory_order_release);
    audio_startup_packets_skipped_.store(0, std::memory_order_release);
    last_decoded_video_ns_ = 0;
    decoded_video_frames_ = 0;
    render_fault_count_ = 0;
    last_presented_video_ns_ = 0;
    consecutive_render_faults_ = 0;
    renderer_stage_ = static_cast<uint8_t>(VideoRenderStage::Idle);
    renderer_stage_started_ns_ = 0;
    if (video_renderer_) {
        lunar::diagnosticLog("media", "shutdown video renderer begin");
        video_renderer_->shutdown();
        lunar::diagnosticLog("media", "shutdown video renderer done");
    }
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
    perf_.store(nullptr, std::memory_order_release);
    lunar::diagnosticLog("media", "shutdown reset components done");
}

bool MediaPipeline::decodeVideoPacket(const uint8_t* data, size_t len,
                                      uint64_t timestamp) {
    if (video_scheduling_.load(std::memory_order_acquire) ==
        VideoSchedulingMode::DirectLowLatency) {
        return decodeVideoDirect(data, len, timestamp);
    }
    return enqueueVideoPacket(data, len, timestamp);
}

bool MediaPipeline::decodeVideoDirect(const uint8_t* data,
                                      size_t len,
                                      uint64_t timestamp) {
    if (!data || len == 0 || len > kMaxVideoQueueBytes) return false;

    bool decoded = false;
    bool reset = true;
    {
        std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
        if (!running_.load() || !video_decoder_) return false;
        decoded = video_decoder_->decode(data, len, timestamp);
        if (!decoded) reset = resetVideoDecoderForKeyframe();
    }
    if (!decoded) {
        lunar::diagnosticLog("media",
                             "direct video decoder recovery reset=%s",
                             reset ? "true" : "false");
        requestVideoRecovery("direct video decoder error");
    }
    return decoded;
}

bool MediaPipeline::decodeAudioPacket(const uint8_t* data, size_t len,
                                      uint16_t sequence,
                                      uint64_t timestamp) {
    return enqueueAudioPacket(data, len, sequence, timestamp);
}

void MediaPipeline::setVideoReadyCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(video_ready_callback_mutex_);
    video_ready_callback_ = std::move(callback);
}

void MediaPipeline::clearVideoRecoveryRequest() {
    std::lock_guard<std::mutex> lock(video_queue_mutex_);
    if (!video_waiting_for_keyframe_.load()) {
        video_recovery_request_ = false;
    }
}

void MediaPipeline::requestVideoRecovery(const char* reason,
                                         bool reset_decoder) {
    if (!running_) return;
    if (reset_decoder) {
        beginHardVideoRecovery(reason);
        return;
    }
    const bool was_pending = video_recovery_request_.exchange(true);
    if (!was_pending) {
        lunar::diagnosticLog("media", "video keyframe requested reason=%s",
                             reason ? reason : "unknown");
    }
}

void MediaPipeline::requestRendererRecovery(const char* reason) {
    if (!running_.load()) return;
    bool expected = false;
    if (!video_renderer_recovery_pending_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        if (video_worker_stop_ || !running_.load()) {
            video_renderer_recovery_pending_ = false;
            return;
        }
        video_renderer_recovery_wakeup_ = true;
    }
    const auto health = getHealthStats();
    lunar::persistentEventLog(
        "video-render",
        "recovery phase=requested reason=%s render_stage=%s "
        "render_stage_age_ms=%llu",
        reason ? reason : "unknown",
        videoRenderStageName(health.renderer_stage),
        static_cast<unsigned long long>(health.renderer_stage_age_ms));
    video_queue_cv_.notify_one();
}

void MediaPipeline::prepareForNewVideoSource(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    if (!running_.load()) return;

    {
        std::lock_guard<std::mutex> video_lock(video_queue_mutex_);
        video_renderer_recovery_pending_ = false;
        video_renderer_recovery_wakeup_ = false;
    }
    video_new_source_pending_ = true;
    last_decoded_video_ns_ = 0;
    decoded_video_frames_ = 0;
    render_fault_count_ = 0;
    last_presented_video_ns_ = 0;
    consecutive_render_faults_ = 0;
    if (video_renderer_) video_renderer_->resetLiveness();
    if (av_sync_) {
        av_sync_->reset();
        av_sync_->start();
    }
    beginHardVideoRecovery(reason ? reason : "new video source", true);
}

void MediaPipeline::prepareForNewMediaSource(const char* reason) {
    {
        std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
        if (!running_.load()) return;

        audio_start_gate_open_.store(false, std::memory_order_release);
        audio_start_primed_.store(false, std::memory_order_release);
        audio_startup_packets_skipped_.store(0, std::memory_order_release);
        video_ready_notified_.store(false, std::memory_order_release);
        audio_source_epoch_.fetch_add(1, std::memory_order_acq_rel);
        audio_source_reset_pending_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> audio_lock(audio_queue_mutex_);
            audio_queue_.clear();
            decoded_audio_queue_.clear();
            queued_audio_bytes_ = 0;
            queued_decoded_audio_bytes_ = 0;
            last_decoded_audio_end_ns_.store(0, std::memory_order_release);
        }
    }
    audio_queue_cv_.notify_one();
    prepareForNewVideoSource(reason ? reason : "new media source");
}

bool MediaPipeline::beginHardVideoRecoveryLocked(bool force_new_epoch,
                                                 size_t& dropped_packets,
                                                 size_t& dropped_bytes) {
    BoundedVideoRecoveryState recovery_state{
        video_recovery_epoch_.load(),
        video_decoder_reset_pending_.load(),
        video_waiting_for_keyframe_.load(),
        video_recovery_request_.load(),
        video_decoder_reset_wakeup_,
    };
    if (!applyBoundedVideoRecovery(recovery_state,
                                   running_.load(),
                                   video_worker_stop_,
                                   force_new_epoch)) {
        video_recovery_request_ = recovery_state.recovery_request;
        return false;
    }

    dropped_packets = video_queue_.size();
    dropped_bytes = queued_video_bytes_;
    video_queue_.clear();
    queued_video_bytes_ = 0;
    video_recovery_epoch_ = recovery_state.epoch;
    video_decoder_reset_pending_ = recovery_state.reset_pending;
    video_renderer_recovery_pending_ = false;
    video_renderer_recovery_wakeup_ = false;
    video_waiting_for_keyframe_ = recovery_state.waiting_for_keyframe;
    video_recovery_request_ = recovery_state.recovery_request;
    video_decoder_reset_wakeup_ = recovery_state.reset_wakeup;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    recordVideoQueueLocked();
#endif
    return true;
}

void MediaPipeline::beginHardVideoRecovery(const char* reason,
                                           bool force_new_epoch) {
    if (!running_) return;
    const bool was_pending = video_recovery_request_.exchange(true);
    size_t dropped_packets = 0;
    size_t dropped_bytes = 0;
    bool started = false;
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        started = beginHardVideoRecoveryLocked(force_new_epoch,
                                               dropped_packets,
                                               dropped_bytes);
    }
    if (!was_pending || started) {
        lunar::diagnosticLog(
            "media",
            "video hard recovery reason=%s epoch=%u dropped=%zu bytes=%zu reset=%s",
            reason ? reason : "unknown",
            video_recovery_epoch_.load(),
            dropped_packets,
            dropped_bytes,
            started ? "true" : "already_pending");
    }
    auto* perf = perfStats();
    if (started && dropped_packets > 0 && perf) {
        perf->recordVideoQueueDrops(static_cast<uint32_t>(dropped_packets));
        perf->logVideoDropDiagnostic(
            "recovery_epoch",
            reason ? reason : "hard_video_recovery",
            0,
            0,
            0,
            dropped_bytes);
    }
    if (started) video_queue_cv_.notify_one();
}

bool MediaPipeline::enqueueVideoPacket(const uint8_t* data,
                                       size_t len,
                                       uint64_t timestamp) {
    if (!running_ || !data || len == 0 || len > kMaxVideoQueueBytes) return false;

    const bool bounded = video_scheduling_.load(std::memory_order_acquire) ==
        VideoSchedulingMode::BoundedLowLatency;

    QueuedVideoPacket packet;
    packet.timestamp = timestamp;
    packet.generation = generation_.load();
    packet.access_unit = video_decoder_
        ? video_decoder_->inspectAccessUnit(data, len)
        : VideoAccessUnitInfo{};
    packet.contains_idr = packet.access_unit.has_random_access;
    packet.contains_vcl = packet.access_unit.has_vcl;
    packet.enqueued_at = std::chrono::steady_clock::now();

    // Bounded PS ingress must avoid copying access units that are already
    // known to be unusable. The full admission check below remains required
    // because queue depth and age can change while the AU is being copied.
    bool drop_before_copy = false;
    bool recovery_started_before_copy = false;
    bool recovery_was_pending_before_copy = false;
    size_t recovery_dropped_packets_before_copy = 0;
    size_t recovery_dropped_bytes_before_copy = 0;
    if (bounded) {
        {
            std::lock_guard<std::mutex> lock(video_queue_mutex_);
            if (video_worker_stop_ || !running_) return false;
            if (packet.generation != generation_.load()) return false;

            const auto pre_copy_admission = evaluateBoundedVideoAdmission(
                {0, 0, std::chrono::steady_clock::duration::zero(),
                 video_waiting_for_keyframe_.load()},
                len, packet.contains_idr, packet.contains_vcl,
                video_queue_limits_.max_packets,
                video_queue_limits_.max_bytes, video_queue_limits_.max_age);
            if (pre_copy_admission == BoundedVideoAdmission::DropDependent) {
                bounded_video_stats_.intentional_drop++;
                maybeLogBoundedVideoStatsLocked(
                    std::chrono::steady_clock::now());
                drop_before_copy = true;
            } else if (pre_copy_admission ==
                       BoundedVideoAdmission::RejectOversize) {
                recovery_was_pending_before_copy =
                    video_recovery_request_.exchange(true);
                recovery_started_before_copy = beginHardVideoRecoveryLocked(
                    true,
                    recovery_dropped_packets_before_copy,
                    recovery_dropped_bytes_before_copy);
                if (recovery_started_before_copy) {
                    bounded_video_stats_.recovery_overflow++;
                    bounded_video_stats_.idr_requests++;
                }
                bounded_video_stats_.intentional_drop++;
                maybeLogBoundedVideoStatsLocked(
                    std::chrono::steady_clock::now());
                drop_before_copy = true;
            }
        }
        if (drop_before_copy) {
            if (recovery_started_before_copy) {
                if (auto* perf = perfStats()) {
                    perf->recordVideoQueueDrops(
                        static_cast<uint32_t>(
                            recovery_dropped_packets_before_copy));
                    perf->logVideoDropDiagnostic(
                        "queue_overflow",
                        "encoded_video_queue_limit",
                        0,
                        0,
                        timestamp,
                        len);
                }
                if (!recovery_was_pending_before_copy ||
                    shouldLogMediaQueue()) {
                    lunar::diagnosticLog(
                        "media",
                        "video hard recovery reason=video access unit oversize "
                        "epoch=%u dropped=%zu bytes=%zu",
                        video_recovery_epoch_.load(),
                        recovery_dropped_packets_before_copy,
                        recovery_dropped_bytes_before_copy);
                }
                video_queue_cv_.notify_one();
            }
            return true;
        }
    }

    const auto copy_started = packet.enqueued_at;
    try {
        packet.data.assign(data, data + len);
    } catch (...) {
        if (bounded) {
            std::lock_guard<std::mutex> lock(video_queue_mutex_);
            bounded_video_stats_.alloc_fail++;
            maybeLogBoundedVideoStatsLocked(std::chrono::steady_clock::now());
        }
        lunar::diagnosticLog("media", "video queue alloc failed len=%zu", len);
        return false;
    }
    const auto copied_at = std::chrono::steady_clock::now();
    const auto copy_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            copied_at - copy_started).count());

    bool recovery_started = false;
    bool recovery_was_pending = false;
    size_t recovery_dropped_packets = 0;
    size_t recovery_dropped_bytes = 0;
    bool intentional_drop = false;
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        if (video_worker_stop_ || !running_) return false;

        if (packet.generation != generation_.load()) return false;

        const auto now = std::chrono::steady_clock::now();
        const auto oldest_age = video_queue_.empty()
            ? std::chrono::steady_clock::duration::zero()
            : now - video_queue_.front().enqueued_at;
        const size_t max_packets = bounded
            ? video_queue_limits_.max_packets
            : kMaxVideoQueuePackets;
        const size_t max_bytes = bounded
            ? video_queue_limits_.max_bytes
            : kMaxVideoQueueBytes;
        const auto admission = bounded
            ? evaluateBoundedVideoAdmission(
                  {video_queue_.size(), std::min(queued_video_bytes_, max_bytes),
                   oldest_age, video_waiting_for_keyframe_.load()},
                  packet.data.size(), packet.contains_idr, packet.contains_vcl,
                  max_packets, max_bytes,
                  video_queue_limits_.max_age)
            : BoundedVideoAdmission::Accept;
        const bool age_exceeded = admission == BoundedVideoAdmission::RecoverAge;
        const bool capacity_exceeded =
            admission == BoundedVideoAdmission::RecoverOverflow ||
            admission == BoundedVideoAdmission::RejectOversize ||
            (!bounded && realtimeVideoCapacityExceeded(
                video_queue_.size(), queued_video_bytes_, packet.data.size(),
                max_packets, max_bytes));

        if (admission == BoundedVideoAdmission::DropDependent) {
            intentional_drop = true;
        } else if (age_exceeded || capacity_exceeded) {
            recovery_was_pending = video_recovery_request_.exchange(true);
#if LUNARNX_DROP_DIAGNOSTIC_LOG
            recordVideoQueueLocked();
#endif
            recovery_started = beginHardVideoRecoveryLocked(
                true,
                recovery_dropped_packets,
                recovery_dropped_bytes);
            if (bounded && recovery_started) {
                if (age_exceeded) bounded_video_stats_.recovery_age++;
                else bounded_video_stats_.recovery_overflow++;
                bounded_video_stats_.idr_requests++;
            }
            if (auto* perf = perfStats()) {
                perf->recordVideoQueueDrops(
                    static_cast<uint32_t>(recovery_dropped_packets));
                perf->logVideoDropDiagnostic(
                    "queue_overflow",
                    "encoded_video_queue_limit",
                    0,
                    0,
                    timestamp,
                    len);
            }
            if (shouldLogMediaQueue()) {
                lunar::diagnosticLog("media",
                                     "video queue reset dropped=%zu packets=%zu bytes=%zu",
                                     recovery_dropped_packets,
                                     recovery_dropped_packets,
                                     recovery_dropped_bytes);
            }
            if (!packet.contains_idr ||
                admission == BoundedVideoAdmission::RejectOversize) {
                intentional_drop = true;
            }
        }
        if (!intentional_drop &&
            (!bounded || boundedVideoAdmissionMayEnqueue(admission))) {
            packet.recovery_epoch = video_recovery_epoch_.load();
            video_queue_.push_back(std::move(packet));
            queued_video_bytes_ += video_queue_.back().data.size();
            if (bounded) {
                bounded_video_stats_.enqueued++;
                bounded_video_stats_.enqueue_copy_total_us += copy_us;
                bounded_video_stats_.enqueue_copy_max_us = std::max(
                    bounded_video_stats_.enqueue_copy_max_us, copy_us);
                bounded_video_stats_.depth_high = std::max(
                    bounded_video_stats_.depth_high, video_queue_.size());
                bounded_video_stats_.bytes_high = std::max(
                    bounded_video_stats_.bytes_high, queued_video_bytes_);
            }
        } else if (bounded) {
            bounded_video_stats_.intentional_drop++;
        }
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        recordVideoQueueLocked();
#endif
        if (!intentional_drop && shouldLogMediaQueue()) {
            lunar::diagnosticLog("media",
                                 "video queue push len=%zu packets=%zu bytes=%zu",
                                 len,
                                 video_queue_.size(),
                                 queued_video_bytes_);
        }
        if (bounded) maybeLogBoundedVideoStatsLocked(copied_at);
    }
    if (recovery_started && (!recovery_was_pending || shouldLogMediaQueue())) {
        lunar::diagnosticLog("media",
                             "video hard recovery reason=video queue overflow epoch=%u",
                             video_recovery_epoch_.load());
    }
    if (!intentional_drop) video_queue_cv_.notify_one();
    // A bounded drop is an intentional client scheduling decision, not a
    // Chiaki transport callback failure. Recovery state already owns the IDR.
    return true;
}

void MediaPipeline::maybeLogBoundedVideoStatsLocked(
    std::chrono::steady_clock::time_point now) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    if (video_scheduling_.load(std::memory_order_relaxed) !=
        VideoSchedulingMode::BoundedLowLatency) return;
    if (bounded_video_stats_.window_start.time_since_epoch().count() == 0) {
        bounded_video_stats_.window_start = now;
        return;
    }
    if (now - bounded_video_stats_.window_start < std::chrono::seconds(10)) return;

    const uint64_t copy_avg = bounded_video_stats_.enqueued
        ? bounded_video_stats_.enqueue_copy_total_us / bounded_video_stats_.enqueued
        : 0;
    const uint64_t decode_avg = bounded_video_stats_.decoded
        ? bounded_video_stats_.worker_decode_total_us / bounded_video_stats_.decoded
        : 0;
    lunar::dropDiagnosticLog(
        "LUNARNX-PSQ",
        "mode=bounded enqueued=%llu decoded=%llu intentional_drop=%llu "
        "alloc_fail=%llu depth_now=%zu depth_high=%zu bytes_now=%zu "
        "bytes_high=%zu oldest_age_max_us=%llu enqueue_copy_avg_us=%llu "
        "enqueue_copy_max_us=%llu worker_decode_avg_us=%llu "
        "worker_decode_max_us=%llu recovery_overflow=%llu recovery_age=%llu "
        "recovery_decode=%llu idr_requests=%llu waiting_idr=%d",
        static_cast<unsigned long long>(bounded_video_stats_.enqueued),
        static_cast<unsigned long long>(bounded_video_stats_.decoded),
        static_cast<unsigned long long>(bounded_video_stats_.intentional_drop),
        static_cast<unsigned long long>(bounded_video_stats_.alloc_fail),
        video_queue_.size(), bounded_video_stats_.depth_high,
        queued_video_bytes_, bounded_video_stats_.bytes_high,
        static_cast<unsigned long long>(bounded_video_stats_.oldest_age_max_us),
        static_cast<unsigned long long>(copy_avg),
        static_cast<unsigned long long>(bounded_video_stats_.enqueue_copy_max_us),
        static_cast<unsigned long long>(decode_avg),
        static_cast<unsigned long long>(bounded_video_stats_.worker_decode_max_us),
        static_cast<unsigned long long>(bounded_video_stats_.recovery_overflow),
        static_cast<unsigned long long>(bounded_video_stats_.recovery_age),
        static_cast<unsigned long long>(bounded_video_stats_.recovery_decode),
        static_cast<unsigned long long>(bounded_video_stats_.idr_requests),
        video_waiting_for_keyframe_.load() ? 1 : 0);

    bounded_video_stats_ = {};
    bounded_video_stats_.window_start = now;
#else
    (void)now;
#endif
}

#if LUNARNX_DROP_DIAGNOSTIC_LOG
void MediaPipeline::recordVideoQueueLocked() {
    auto* perf = perfStats();
    if (!perf) return;
    uint32_t oldest_age_ms = 0;
    if (!video_queue_.empty()) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - video_queue_.front().enqueued_at).count();
        if (age > 0) oldest_age_ms = static_cast<uint32_t>(age);
    }
    perf->recordVideoQueue(static_cast<uint32_t>(video_queue_.size()),
                           queued_video_bytes_,
                           oldest_age_ms);
}
#endif

bool MediaPipeline::enqueueAudioPacket(const uint8_t* data,
                                       size_t len,
                                       uint16_t sequence,
                                       uint64_t timestamp) {
    if (!running_ || !data || len == 0) return false;
    // Xbox can send roughly a second of audio while video remains gated on
    // media startup and its first clean IDR. Playing that FIFO later makes
    // sound permanently trail the live picture. Start at the next Opus packet
    // after the renderer has successfully presented its first frame instead.
    if (!audio_start_gate_open_.load(std::memory_order_acquire)) {
        audio_startup_packets_skipped_.fetch_add(1,
                                                 std::memory_order_relaxed);
        return true;
    }
    auto* perf = perfStats();
    if (len > kMaxAudioQueueBytes) {
        lunar::diagnosticLog("media", "audio packet too large len=%zu", len);
        if (perf) perf->recordAudioDrop();
        return false;
    }

    EncodedAudioPacket packet;
    packet.sequence = sequence;
    packet.timestamp = timestamp;
    packet.generation = generation_.load();
    packet.source_epoch = audio_source_epoch_.load(std::memory_order_acquire);
    try {
        packet.data.assign(data, data + len);
    } catch (...) {
        lunar::diagnosticLog("media", "audio queue alloc failed len=%zu", len);
        if (perf) perf->recordAudioDrop();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (audio_worker_stop_ || !running_) return false;
        const size_t live_packet_limit = audioIngressQueuePacketLimit(
            audio_latency_mode_.load(std::memory_order_acquire));
        if (audio_queue_.size() >= live_packet_limit) {
            const size_t dropped_packets = audio_queue_.size();
            audio_queue_.clear();
            queued_audio_bytes_ = 0;
            audio_catch_up_pending_ = true;
            // The queue keeps the newest packet below. Resume it immediately
            // after resetting the Opus/reorder timeline; waiting for another
            // full startup prebuffer would extend an existing network gap.
            audio_start_primed_.store(true, std::memory_order_release);
            if (perf) {
                for (size_t i = 0; i < dropped_packets; ++i) {
                    perf->recordAudioDrop();
                }
            }
            if (shouldLogMediaQueue()) {
                lunar::diagnosticLog(
                    "media",
                    "audio live-edge catch-up dropped=%zu limit=%zu mode=%s",
                    dropped_packets,
                    live_packet_limit,
                    audioLatencyModeName(
                        audio_latency_mode_.load(std::memory_order_relaxed)));
            }
        }
        while (!audio_queue_.empty() &&
               (audio_queue_.size() >= kMaxAudioQueuePackets ||
                queued_audio_bytes_ + packet.data.size() > kMaxAudioQueueBytes)) {
            queued_audio_bytes_ -= audio_queue_.front().data.size();
            audio_queue_.pop_front();
            if (perf) perf->recordAudioDrop();
            if (shouldLogMediaQueue()) {
                lunar::diagnosticLog("media",
                                     "audio queue drop packets=%zu bytes=%zu",
                                     audio_queue_.size(),
                                     queued_audio_bytes_);
            }
        }
        audio_queue_.push_back(std::move(packet));
        queued_audio_bytes_ += audio_queue_.back().data.size();
        if (!audio_start_primed_.load(std::memory_order_relaxed)) {
            const size_t prebuffer_packets = audioStartupPrebufferPackets(
                audio_latency_mode_.load(std::memory_order_acquire));
            if (audio_queue_.size() >= prebuffer_packets) {
                audio_start_primed_.store(true, std::memory_order_release);
                lunar::diagnosticLog(
                    "media",
                    "audio live-edge primed packets=%zu target=%zu",
                    audio_queue_.size(), prebuffer_packets);
            }
        }
    }
    audio_queue_cv_.notify_one();
    return true;
}

bool MediaPipeline::playDecodedAudio(const AudioFrame& frame) {
    auto* perf = perfStats();
    if (!running_.load() || frame.sample_rate != 48000 || frame.channels != 2 ||
        frame.sample_count == 0 || frame.pcm_data.empty() ||
        frame.sample_count > SIZE_MAX / (2 * sizeof(int16_t)) ||
        frame.pcm_data.size() != frame.sample_count * 2 * sizeof(int16_t) ||
        frame.pcm_data.size() > kMaxAudioQueueBytes) {
        if (perf) perf->recordAudioDrop();
        return false;
    }

    QueuedDecodedAudio queued;
    queued.generation = generation_.load();
    queued.source_epoch = audio_source_epoch_.load(std::memory_order_acquire);
    try {
        queued.frame = frame;
    } catch (...) {
        if (perf) perf->recordAudioDrop();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (audio_worker_stop_ || !running_.load()) return false;
        while (!decoded_audio_queue_.empty() &&
               (decoded_audio_queue_.size() >= kMaxAudioQueuePackets ||
                queued_decoded_audio_bytes_ + queued.frame.pcm_data.size() >
                    kMaxAudioQueueBytes)) {
            queued_decoded_audio_bytes_ -=
                decoded_audio_queue_.front().frame.pcm_data.size();
            decoded_audio_queue_.pop_front();
            if (perf) perf->recordAudioDrop();
        }
        if (queued_decoded_audio_bytes_ + queued.frame.pcm_data.size() >
            kMaxAudioQueueBytes) {
            if (perf) perf->recordAudioDrop();
            return false;
        }
        queued_decoded_audio_bytes_ += queued.frame.pcm_data.size();
        decoded_audio_queue_.push_back(std::move(queued));
    }
    audio_queue_cv_.notify_one();
    return true;
}

void MediaPipeline::recordIncomingVideoSample(size_t bytes, uint64_t pts_ns,
                                               uint32_t frames_lost) {
    auto* perf = perfStats();
    if (!perf) return;
    perf->recordVideoPacket(bytes, pts_ns);
    perf->recordVideoNetworkBytes(bytes);
    perf->recordPackets(1, frames_lost);
}

void MediaPipeline::recordIncomingAudioPacket() {
    auto* perf = perfStats();
    if (!perf) return;
    perf->recordAudioPacket();
}

bool MediaPipeline::startWorkers(uint32_t generation) {
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        video_worker_stop_ = false;
        video_worker_generation_ = generation;
        video_queue_.clear();
        queued_video_bytes_ = 0;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        recordVideoQueueLocked();
#endif
        video_recovery_request_ = false;
        video_decoder_reset_pending_ = false;
        video_renderer_recovery_pending_ = false;
        video_renderer_recovery_in_progress_ = false;
        video_new_source_pending_ = false;
        video_waiting_for_keyframe_ = false;
        video_recovery_epoch_ = 0;
        video_decoder_reset_wakeup_ = false;
        video_renderer_recovery_wakeup_ = false;
        bounded_video_stats_ = {};
        bounded_video_stats_.window_start = std::chrono::steady_clock::now();
    }
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_worker_stop_ = false;
        audio_worker_generation_ = generation;
        audio_queue_.clear();
        decoded_audio_queue_.clear();
        queued_audio_bytes_ = 0;
        queued_decoded_audio_bytes_ = 0;
        last_decoded_audio_end_ns_.store(0, std::memory_order_release);
        audio_source_reset_pending_.store(false, std::memory_order_release);
        audio_catch_up_pending_ = false;
        audio_decode_epoch_.store(audio_source_epoch_.load(
                                       std::memory_order_acquire),
                                   std::memory_order_release);
    }
    g_media_queue_logs = 0;
    g_media_worker_logs = 0;
    g_video_sync_logs = 0;
    g_cloud_1080_render_frames = 0;

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
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        recordVideoQueueLocked();
#endif
        video_worker_generation_ = 0;
        video_worker_stop_ = false;
        video_decoder_reset_pending_ = false;
        video_renderer_recovery_pending_ = false;
        video_renderer_recovery_in_progress_ = false;
        video_new_source_pending_ = false;
        video_waiting_for_keyframe_ = false;
        video_recovery_epoch_ = 0;
        video_decoder_reset_wakeup_ = false;
        video_renderer_recovery_wakeup_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_queue_.clear();
        decoded_audio_queue_.clear();
        queued_audio_bytes_ = 0;
        queued_decoded_audio_bytes_ = 0;
        audio_worker_generation_ = 0;
        audio_worker_stop_ = false;
        last_decoded_audio_end_ns_.store(0, std::memory_order_release);
        audio_source_reset_pending_.store(false, std::memory_order_release);
        audio_catch_up_pending_ = false;
        audio_decode_epoch_.store(audio_source_epoch_.load(
                                       std::memory_order_acquire),
                                   std::memory_order_release);
    }
}

void MediaPipeline::videoWorkerLoop() {
    lunar::diagnosticLog("media", "video worker loop begin generation=%u",
                         video_worker_generation_);
    bool decode_catch_up_active = false;
    while (true) {
        QueuedVideoPacket packet;
        bool have_packet = false;
        bool reset_requested = false;
        bool reset_new_source = false;
        bool renderer_recovery_requested = false;
        size_t packet_queued_behind = 0;
        uint32_t reset_epoch = 0;
        {
            std::unique_lock<std::mutex> lock(video_queue_mutex_);
            video_queue_cv_.wait(lock, [this]() {
                return video_worker_stop_ || !video_queue_.empty() ||
                       video_decoder_reset_wakeup_ ||
                       video_renderer_recovery_wakeup_;
            });
            if (video_worker_stop_) break;
            const auto now = std::chrono::steady_clock::now();
            if (video_scheduling_.load(std::memory_order_acquire) ==
                    VideoSchedulingMode::BoundedLowLatency &&
                !video_queue_.empty() &&
                now - video_queue_.front().enqueued_at >=
                    video_queue_limits_.max_age) {
                size_t dropped_packets = 0;
                size_t dropped_bytes = 0;
                if (beginHardVideoRecoveryLocked(true,
                                                 dropped_packets,
                                                 dropped_bytes)) {
                    bounded_video_stats_.recovery_age++;
                    bounded_video_stats_.idr_requests++;
                }
                decode_catch_up_active = false;
                maybeLogBoundedVideoStatsLocked(now);
                continue;
            }
            renderer_recovery_requested = video_renderer_recovery_wakeup_;
            video_renderer_recovery_wakeup_ = false;
            const bool reset_wakeup = !renderer_recovery_requested &&
                video_decoder_reset_wakeup_;
            if (reset_wakeup) video_decoder_reset_wakeup_ = false;
            if (!renderer_recovery_requested && !video_queue_.empty()) {
                packet = std::move(video_queue_.front());
                video_queue_.pop_front();
                queued_video_bytes_ -= packet.data.size();
                have_packet = true;
                packet_queued_behind = video_queue_.size();
            }
            reset_requested = boundedVideoResetMustPrecedeDecode(
                video_decoder_reset_pending_.load(), reset_wakeup,
                have_packet && packet.contains_idr);
            reset_new_source = video_new_source_pending_.load();
            reset_epoch = video_recovery_epoch_.load();
            if (have_packet) {
                packet.queue_age_us = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - packet.enqueued_at).count());
                bounded_video_stats_.oldest_age_max_us = std::max(
                    bounded_video_stats_.oldest_age_max_us,
                    packet.queue_age_us);
            }
#if LUNARNX_DROP_DIAGNOSTIC_LOG
            recordVideoQueueLocked();
#endif
        }
        if (renderer_recovery_requested) {
            decode_catch_up_active = false;
            video_renderer_recovery_in_progress_.store(
                true, std::memory_order_release);
            const auto recovery_started = std::chrono::steady_clock::now();
            const bool recovered = video_renderer_ &&
                video_renderer_->prepareDecoderReset();
            const auto handoff_ms = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - recovery_started).count();
            const auto health = getHealthStats();
            if (recovered) {
                video_renderer_recovery_pending_ = false;
                lunar::persistentEventLog(
                    "video-render",
                    "recovery phase=gpu-fences-retired action=resume "
                    "handoff_ms=%lld render_stage=%s render_stage_age_ms=%llu",
                    static_cast<long long>(handoff_ms),
                    videoRenderStageName(health.renderer_stage),
                    static_cast<unsigned long long>(
                        health.renderer_stage_age_ms));
            } else {
                lunar::persistentEventLog(
                    "video-render",
                    "recovery phase=gpu-handoff-failed action=watchdog-grace "
                    "handoff_ms=%lld render_stage=%s render_stage_age_ms=%llu",
                    static_cast<long long>(handoff_ms),
                    videoRenderStageName(health.renderer_stage),
                    static_cast<unsigned long long>(
                        health.renderer_stage_age_ms));
            }
            video_renderer_recovery_in_progress_.store(
                false, std::memory_order_release);
            continue;
        }
        if (reset_requested) {
            decode_catch_up_active = false;
            {
                std::lock_guard<std::mutex> suppression_lock(
                    video_suppression_mutex_);
                suppressed_video_timestamps_.clear();
            }
            const bool reset = video_decoder_ && (reset_new_source
                ? resetVideoDecoderForKeyframe(true)
                : resetVideoDecoderForKeyframe());
            if (!reset) {
                lunar::diagnosticLog("media", "video decoder reset for keyframe failed");
                video_recovery_request_ = true;
                continue;
            } else {
                std::lock_guard<std::mutex> lock(video_queue_mutex_);
                if (video_recovery_epoch_.load() == reset_epoch) {
                    video_decoder_reset_pending_ = false;
                    if (reset_new_source) video_new_source_pending_ = false;
                }
            }
        }
        if (!have_packet) continue;
        const auto catch_up = videoDecodeCatchUpDecision(
            video_decode_catch_up_mode_.load(std::memory_order_acquire),
            decode_catch_up_active,
            packet_queued_behind,
            packet.queue_age_us);
        decode_catch_up_active = catch_up.active;
        packet.suppress_output = catch_up.suppress_output;
        if (video_scheduling_.load(std::memory_order_acquire) ==
                VideoSchedulingMode::BoundedLowLatency &&
            std::chrono::steady_clock::now() - packet.enqueued_at >=
                video_queue_limits_.max_age) {
            beginHardVideoRecovery("video queue age", true);
            {
                std::lock_guard<std::mutex> lock(video_queue_mutex_);
                bounded_video_stats_.recovery_age++;
                bounded_video_stats_.idr_requests++;
                maybeLogBoundedVideoStatsLocked(std::chrono::steady_clock::now());
            }
            continue;
        }
        if (shouldLogMediaWorker()) {
            lunar::diagnosticLog("media", "video worker pop len=%zu", packet.data.size());
        }
        processVideoPacket(packet);
    }
    lunar::diagnosticLog("media", "video worker loop end");
}

bool MediaPipeline::resetVideoDecoderForKeyframe(bool new_source) {
    if (!video_decoder_) return false;
    if (video_renderer_ && !video_renderer_->prepareDecoderReset()) {
        lunar::dropDiagnosticLog(
            "video-reset", "phase=gpu-handoff-failed action=defer-decoder-flush");
        return false;
    }
    return new_source ? video_decoder_->resetForNewSource()
                      : video_decoder_->resetForKeyframe();
}

void MediaPipeline::audioWorkerLoop() {
    lunar::diagnosticLog("media", "audio worker loop begin generation=%u",
                         audio_worker_generation_);
    AudioPacketReorder reorder;
    uint32_t audio_worker_source_epoch =
        audio_source_epoch_.load(std::memory_order_acquire);
    while (true) {
        EncodedAudioPacket packet;
        QueuedDecodedAudio decoded;
        bool have_encoded = false;
        bool have_decoded = false;
        bool reset_audio = false;
        bool catch_up_audio = false;
        uint32_t current_source_epoch = audio_worker_source_epoch;
        {
            std::unique_lock<std::mutex> lock(audio_queue_mutex_);
            audio_queue_cv_.wait(lock, [this]() {
                return audio_worker_stop_ ||
                       (audio_start_primed_.load(std::memory_order_acquire) &&
                        !audio_queue_.empty()) ||
                       !decoded_audio_queue_.empty() ||
                       audio_catch_up_pending_ ||
                       audio_source_reset_pending_.load(
                           std::memory_order_acquire);
            });
            if (audio_worker_stop_) break;
            current_source_epoch =
                audio_source_epoch_.load(std::memory_order_acquire);
            catch_up_audio = audio_catch_up_pending_;
            audio_catch_up_pending_ = false;
            reset_audio = audio_source_reset_pending_.exchange(
                false, std::memory_order_acq_rel) ||
                current_source_epoch != audio_worker_source_epoch;
            if (!reset_audio && !catch_up_audio &&
                !decoded_audio_queue_.empty()) {
                decoded = std::move(decoded_audio_queue_.front());
                decoded_audio_queue_.pop_front();
                queued_decoded_audio_bytes_ -= decoded.frame.pcm_data.size();
                have_decoded = true;
            } else if (!reset_audio && !catch_up_audio &&
                       !audio_queue_.empty()) {
                packet = std::move(audio_queue_.front());
                audio_queue_.pop_front();
                queued_audio_bytes_ -= packet.data.size();
                have_encoded = true;
            }
        }

        if (reset_audio) {
            reorder.reset();
            if (audio_decoder_) audio_decoder_->reset();
            if (audio_player_) audio_player_->flush();
            if (av_sync_) av_sync_->invalidateAudioClock();
            audio_worker_source_epoch = current_source_epoch;
            audio_decode_epoch_.store(audio_worker_source_epoch,
                                      std::memory_order_release);
            last_decoded_audio_end_ns_.store(0, std::memory_order_release);
            continue;
        }

        if (catch_up_audio) {
            // Abandon the encoded/reorder timeline, but preserve PCM that is
            // already queued in Audren. Flushing the hardware ring here turns
            // a recoverable worker/network burst into an audible hard gap.
            reorder.reset();
            if (audio_decoder_) audio_decoder_->reset();
            last_decoded_audio_end_ns_.store(0, std::memory_order_release);
            continue;
        }

        if (have_decoded) {
            if (decoded.source_epoch == audio_worker_source_epoch &&
                decoded.source_epoch ==
                    audio_source_epoch_.load(std::memory_order_acquire)) {
                submitDecodedAudio(decoded.frame,
                                   decoded.generation,
                                   decoded.source_epoch);
            }
            continue;
        }
        if (!have_encoded) continue;

        if (packet.generation != audio_worker_generation_ ||
            packet.source_epoch != audio_worker_source_epoch ||
            packet.source_epoch !=
                audio_source_epoch_.load(std::memory_order_acquire) ||
            !isGenerationActive(packet.generation)) {
            continue;
        }
        audio_decode_epoch_.store(audio_worker_source_epoch,
                                  std::memory_order_release);
        for (auto& action : reorder.push(std::move(packet))) {
            if (!audio_decoder_ || !isGenerationActive(audio_worker_generation_)) break;
            if (action.type == AudioReorderAction::Type::Missing) {
                const uint64_t last_audio_end =
                    last_decoded_audio_end_ns_.load(std::memory_order_acquire);
                if (last_audio_end == 0) {
                    if (auto* perf = perfStats()) perf->recordAudioDrop();
                    continue;
                }
                audio_decoder_->decodeMissing(last_audio_end);
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
    if (packet.data.empty() ||
        !boundedVideoPacketIsCurrent(running_.load(), packet.generation,
                                     video_worker_generation_,
                                     packet.recovery_epoch,
                                     video_recovery_epoch_.load()) ||
        !isGenerationActive(packet.generation)) {
        return;
    }
#if LUNARNX_DROP_DIAGNOSTIC_LOG || LUNARNX_LATENCY_DIAGNOSTIC_LOG
    auto* perf = perfStats();
#endif
    if (!boundedVideoMayDecodeWhileRecovering(
            video_waiting_for_keyframe_.load(), packet.contains_idr,
            packet.contains_vcl)) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        if (perf) {
            perf->logVideoDropDiagnostic(
                "recovery_epoch",
                "waiting_for_idr",
                0,
                0,
                packet.timestamp,
                packet.data.size());
        }
#endif
        return;
    }
#if LUNARNX_DROP_DIAGNOSTIC_LOG || LUNARNX_LATENCY_DIAGNOSTIC_LOG
    if (perf) {
        perf->recordVideoAccessUnit(packet.data.size(),
                                    packet.timestamp,
                                    packet.queue_age_us,
                                    false);
    }
#endif
    if (video_decoder_) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        const uint32_t diagnostic_events_before = perf
            ? perf->dropDiagnosticEventCount()
            : 0;
#endif
        const auto decode_started = std::chrono::steady_clock::now();
        if (packet.suppress_output && packet.contains_vcl) {
            std::lock_guard<std::mutex> suppression_lock(
                video_suppression_mutex_);
            constexpr size_t kMaxSuppressedVideoTimestamps = 64;
            if (suppressed_video_timestamps_.size() >=
                kMaxSuppressedVideoTimestamps) {
                suppressed_video_timestamps_.pop_front();
            }
            suppressed_video_timestamps_.push_back(packet.timestamp);
        }
        const bool decoded = video_decoder_->decode(packet.data.data(),
                                                    packet.data.size(),
                                                    packet.timestamp,
                                                    &packet.access_unit);
        const auto decode_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - decode_started).count());
        if (video_scheduling_.load(std::memory_order_relaxed) ==
            VideoSchedulingMode::BoundedLowLatency) {
            std::lock_guard<std::mutex> lock(video_queue_mutex_);
            if (decoded) bounded_video_stats_.decoded++;
            bounded_video_stats_.worker_decode_total_us += decode_us;
            bounded_video_stats_.worker_decode_max_us = std::max(
                bounded_video_stats_.worker_decode_max_us, decode_us);
            if (!decoded) {
                bounded_video_stats_.recovery_decode++;
                bounded_video_stats_.idr_requests++;
            }
            maybeLogBoundedVideoStatsLocked(std::chrono::steady_clock::now());
        }
        if (decoded && packet.contains_idr) {
            std::lock_guard<std::mutex> lock(video_queue_mutex_);
            if (packet.recovery_epoch == video_recovery_epoch_.load()) {
                video_waiting_for_keyframe_ = false;
                video_recovery_request_ = false;
            }
        } else if (!decoded) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
            if (perf &&
                perf->dropDiagnosticEventCount() == diagnostic_events_before) {
                perf->logVideoDropDiagnostic(
                    "decode_recovery",
                    "decoder_rejected_access_unit",
                    0,
                    0,
                    packet.timestamp,
                    packet.data.size());
            }
#endif
            beginHardVideoRecovery("video decoder error", true);
        }
    }
}

bool MediaPipeline::isGenerationActive(uint32_t generation) const {
    return running_.load() && generation_.load() == generation;
}

void MediaPipeline::handleVideoFrame(const VideoFrame& frame,
                                     uint32_t generation) {
    auto* perf = perfStats();
    const uint64_t probe_frame_index =
        g_cloud_1080_render_frames.fetch_add(1) + 1;
    if (lunar::shouldSampleCloud1080CrashProbe(probe_frame_index)) {
        lunar::cloud1080CrashProbeLog(
            "crash-probe",
            "DEBUG-c1080 phase=renderer-handoff frame=%llu size=%dx%d "
            "format=%d pts_ns=%llu",
            static_cast<unsigned long long>(probe_frame_index),
            frame.width,
            frame.height,
            frame.format,
            static_cast<unsigned long long>(frame.timestamp));
    }
    int64_t delay_ns = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
        if (!isGenerationActive(generation) || !av_sync_ || !video_renderer_) return;

        last_decoded_video_ns_ = monotonicNowNs();
        decoded_video_frames_.fetch_add(1, std::memory_order_relaxed);
        if (perf) perf->recordDecodedResolution(frame.width, frame.height);

        bool suppress_catch_up_frame = false;
        {
            std::lock_guard<std::mutex> suppression_lock(
                video_suppression_mutex_);
            const auto suppressed = std::find(
                suppressed_video_timestamps_.begin(),
                suppressed_video_timestamps_.end(),
                frame.timestamp);
            if (suppressed != suppressed_video_timestamps_.end()) {
                suppressed_video_timestamps_.erase(suppressed);
                suppress_catch_up_frame = true;
            }
        }
        if (suppress_catch_up_frame) {
            if (perf) perf->recordDecodedCatchUpSuppressed();
            return;
        }

        av_sync_->updateVideoPts(frame.timestamp);
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        const auto timing = av_sync_->getVideoTiming(frame.timestamp);
        delay_ns = timing.clamped_delay_ns;
        if (perf) {
            perf->recordVideoTiming(timing.raw_delay_ns,
                                    timing.clamped_delay_ns,
                                    timing.audio_age_ms,
                                    timing.master_pts_ns,
                                    timing.using_audio_master);
        }
#else
        delay_ns = av_sync_->getVideoDelayNs(frame.timestamp);
#endif
    }

    const auto action = videoSyncAction(delay_ns);
    if (action == VideoSyncAction::Drop) {
        if (perf) {
            perf->recordVideoSyncDrop();
            perf->logVideoDropDiagnostic("av_sync",
                                         "video_late_policy_drop",
                                         0,
                                         0,
                                         frame.timestamp,
                                         0,
                                         frame.width,
                                         frame.height);
        }
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
        std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
        if (!isGenerationActive(generation) || !video_renderer_) return;
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        const auto renderer_enqueue_started = std::chrono::steady_clock::now();
#endif
        const bool rendered = video_renderer_->render(frame);
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        if (perf) {
            perf->recordRendererEnqueue(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() -
                    renderer_enqueue_started).count()));
        }
#endif
        if (lunar::shouldSampleCloud1080CrashProbe(probe_frame_index)) {
            lunar::cloud1080CrashProbeLog(
                "crash-probe",
                "DEBUG-c1080 phase=render-queued frame=%llu result=%d",
                static_cast<unsigned long long>(probe_frame_index),
                rendered ? 1 : 0);
        }
        if (rendered && perf) perf->recordFrame();
        // Software presentation publishes synchronously from render(). The
        // hardware path opens this gate from presentVideoFrame() only after a
        // frame has actually reached the display command stream.
        if (rendered && video_renderer_->successfulPresentCount() > 0) {
            openAudioStartupGateIfNeeded();
        }
        if (rendered && !video_ready_notified_.exchange(true)) {
            std::lock_guard<std::mutex> lock(video_ready_callback_mutex_);
            if (video_ready_callback_) video_ready_callback_();
        }
    }
}

void MediaPipeline::handleAudioFrame(const AudioFrame& frame,
                                     uint32_t generation,
                                     uint32_t source_epoch) {
    if (!isGenerationActive(generation) ||
        source_epoch != audio_source_epoch_.load(std::memory_order_acquire)) {
        return;
    }

    last_decoded_audio_end_ns_.store(
        frame.timestamp + audioSamplesToNanoseconds(frame.sample_count,
                                                    frame.sample_rate),
        std::memory_order_release);
    if (!audio_player_ || !av_sync_) return;
    if (!audio_player_->play(frame)) return;

    const uint64_t playback_timestamp = estimateAudioPlaybackTimestamp(
        frame.timestamp,
        frame.sample_count,
        frame.sample_rate,
        audio_player_->queuedSampleCount());
    av_sync_->updateAudioPts(playback_timestamp);
}

bool MediaPipeline::submitDecodedAudio(const AudioFrame& frame,
                                       uint32_t generation,
                                       uint32_t source_epoch) {
    if (!isGenerationActive(generation) ||
        source_epoch != audio_source_epoch_.load(std::memory_order_acquire) ||
        !audio_player_ || !av_sync_) return false;
    if (!audio_player_->play(frame)) return false;
    if (auto* perf = perfStats()) perf->recordAudioFrame();
    const uint64_t playback_ts = estimateAudioPlaybackTimestamp(
        frame.timestamp, frame.sample_count, frame.sample_rate,
        audio_player_->queuedSampleCount());
    av_sync_->updateAudioPts(playback_ts);
    return true;
}

void MediaPipeline::presentVideoFrame() {
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
    const auto present_started = std::chrono::steady_clock::now();
    auto* latency_perf = perfStats();
    const uint32_t unique_present_before = latency_perf
        ? latency_perf->unique_video_frames_submitted.load(
              std::memory_order_relaxed)
        : 0;
#endif
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    if (running_.load() && video_renderer_) {
        const uint64_t successful_present_before =
            video_renderer_->successfulPresentCount();
        video_renderer_->present();
        const uint64_t successful_present_after =
            video_renderer_->successfulPresentCount();
        if (successful_present_after > successful_present_before) {
            openAudioStartupGateIfNeeded();
        }
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        if (latency_perf) {
            const uint32_t unique_present_after =
                latency_perf->unique_video_frames_submitted.load(
                    std::memory_order_relaxed);
            latency_perf->recordPresentCall(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - present_started).count()),
                unique_present_after > unique_present_before);
        }
#endif
        const auto fault = video_renderer_->consumeRenderFault();
        last_presented_video_ns_.store(
            video_renderer_->lastSuccessfulPresentNs(),
            std::memory_order_release);
        consecutive_render_faults_.store(
            video_renderer_->consecutiveRenderFaults(),
            std::memory_order_release);
        if (fault != RenderFault::None) {
            render_fault_count_.fetch_add(1, std::memory_order_relaxed);
            if (fault == RenderFault::CommandFenceTimeout) {
                requestRendererRecovery(renderFaultName(fault));
            } else {
                beginHardVideoRecovery(renderFaultName(fault), true);
            }
        }
        if (successful_present_after > successful_present_before &&
            !video_renderer_recovery_in_progress_.load(
                std::memory_order_acquire) &&
            video_renderer_recovery_pending_.exchange(
                false, std::memory_order_acq_rel)) {
            lunar::persistentEventLog(
                "video-render",
                "recovery phase=late-present-success action=clear-pending");
        }
    }
}

void MediaPipeline::openAudioStartupGateIfNeeded() {
    if (audio_start_gate_open_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    lunar::diagnosticLog(
        "media",
        "audio startup gate opened after first present skipped_packets=%u",
        audio_startup_packets_skipped_.exchange(
            0, std::memory_order_acq_rel));
}

MediaHealthStats MediaPipeline::getHealthStats() const {
    MediaHealthStats stats;
    const uint64_t now_ns = monotonicNowNs();
    const uint64_t decoded_ns = last_decoded_video_ns_.load(
        std::memory_order_acquire);
    stats.has_decoded_video = decoded_video_frames_.load(
        std::memory_order_acquire) > 0 && decoded_ns != 0;
    if (stats.has_decoded_video && now_ns >= decoded_ns) {
        stats.decoded_video_age_ms = (now_ns - decoded_ns) / 1'000'000u;
    }

    const uint64_t presented_ns = last_presented_video_ns_.load(
        std::memory_order_acquire);
    stats.has_presented_video = presented_ns != 0;
    if (stats.has_presented_video && now_ns >= presented_ns) {
        stats.presented_video_age_ms = (now_ns - presented_ns) / 1'000'000u;
    }
    stats.consecutive_render_faults = consecutive_render_faults_.load(
        std::memory_order_acquire);
    stats.render_fault_count = render_fault_count_.load(
        std::memory_order_acquire);
    stats.renderer_stage = static_cast<VideoRenderStage>(
        renderer_stage_.load(std::memory_order_acquire));
    const uint64_t stage_started_ns = renderer_stage_started_ns_.load(
        std::memory_order_acquire);
    if (stage_started_ns != 0 && now_ns >= stage_started_ns) {
        stats.renderer_stage_age_ms =
            (now_ns - stage_started_ns) / 1'000'000u;
    }
    return stats;
}

} // namespace lunar::stream
