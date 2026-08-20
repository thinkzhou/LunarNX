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
        perf_.store(perf, std::memory_order_release);
        video_codec_ = options.video_codec;
        video_path_ = options.video_path;
        video_scheduling_.store(options.video_scheduling,
                                std::memory_order_release);
        video_queue_limits_ = options.video_queue_limits;

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
        video_decoder_->setVideoCodec(options.video_codec);
        video_decoder_->setVideoPath(options.video_path);
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
            handleAudioFrame(frame, generation);
        });
        lunar::diagnosticLog("media", "audio decoder callback set generation=%u",
                             generation);

        audio_player_->setPerfStats(perf);
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
        video_renderer_->setPerfStats(perf);
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
    video_waiting_for_keyframe_ = false;
    video_recovery_epoch_ = 0;
    video_ready_notified_ = false;
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
    packet.access_unit = video_path_ == VideoPipelinePath::Xbox
        ? inspectXboxH264AccessUnit(data, len)
        : inspectVideoAccessUnit(video_codec_, data, len);
    packet.contains_idr = packet.access_unit.has_random_access;
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
                len, packet.contains_idr, video_queue_limits_.max_packets,
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
                  packet.data.size(), packet.contains_idr, max_packets, max_bytes,
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
        video_waiting_for_keyframe_ = false;
        video_recovery_epoch_ = 0;
        video_decoder_reset_wakeup_ = false;
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
        last_decoded_audio_end_ns_ = 0;
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
        video_waiting_for_keyframe_ = false;
        video_recovery_epoch_ = 0;
        video_decoder_reset_wakeup_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_queue_.clear();
        decoded_audio_queue_.clear();
        queued_audio_bytes_ = 0;
        queued_decoded_audio_bytes_ = 0;
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
        bool have_packet = false;
        bool reset_requested = false;
        uint32_t reset_epoch = 0;
        {
            std::unique_lock<std::mutex> lock(video_queue_mutex_);
            video_queue_cv_.wait(lock, [this]() {
                return video_worker_stop_ || !video_queue_.empty() ||
                       video_decoder_reset_wakeup_;
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
                maybeLogBoundedVideoStatsLocked(now);
                continue;
            }
            const bool reset_wakeup = video_decoder_reset_wakeup_;
            video_decoder_reset_wakeup_ = false;
            if (!video_queue_.empty()) {
                packet = std::move(video_queue_.front());
                video_queue_.pop_front();
                queued_video_bytes_ -= packet.data.size();
                have_packet = true;
            }
            reset_requested = boundedVideoResetMustPrecedeDecode(
                video_decoder_reset_pending_.load(), reset_wakeup,
                have_packet && packet.contains_idr);
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
        if (reset_requested) {
            const bool reset = video_decoder_ && resetVideoDecoderForKeyframe();
            if (!reset) {
                lunar::diagnosticLog("media", "video decoder reset for keyframe failed");
                video_recovery_request_ = true;
                continue;
            } else {
                std::lock_guard<std::mutex> lock(video_queue_mutex_);
                if (video_recovery_epoch_.load() == reset_epoch) {
                    video_decoder_reset_pending_ = false;
                }
            }
        }
        if (!have_packet) continue;
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

bool MediaPipeline::resetVideoDecoderForKeyframe() {
    if (!video_decoder_) return false;
    if (video_renderer_ && !video_renderer_->prepareDecoderReset()) {
        lunar::dropDiagnosticLog(
            "video-reset", "phase=gpu-handoff-failed action=defer-decoder-flush");
        return false;
    }
    return video_decoder_->resetForKeyframe();
}

void MediaPipeline::audioWorkerLoop() {
    lunar::diagnosticLog("media", "audio worker loop begin generation=%u",
                         audio_worker_generation_);
    AudioPacketReorder reorder;
    while (true) {
        EncodedAudioPacket packet;
        QueuedDecodedAudio decoded;
        bool have_encoded = false;
        bool have_decoded = false;
        {
            std::unique_lock<std::mutex> lock(audio_queue_mutex_);
            audio_queue_cv_.wait(lock, [this]() {
                return audio_worker_stop_ || !audio_queue_.empty() ||
                       !decoded_audio_queue_.empty();
            });
            if (audio_worker_stop_) break;
            if (!decoded_audio_queue_.empty()) {
                decoded = std::move(decoded_audio_queue_.front());
                decoded_audio_queue_.pop_front();
                queued_decoded_audio_bytes_ -= decoded.frame.pcm_data.size();
                have_decoded = true;
            } else {
                packet = std::move(audio_queue_.front());
                audio_queue_.pop_front();
                queued_audio_bytes_ -= packet.data.size();
                have_encoded = true;
            }
        }

        if (have_decoded) {
            submitDecodedAudio(decoded.frame, decoded.generation);
            continue;
        }
        if (!have_encoded) continue;

        if (packet.generation != audio_worker_generation_ ||
            !isGenerationActive(packet.generation)) {
            continue;
        }
        for (auto& action : reorder.push(std::move(packet))) {
            if (!audio_decoder_ || !isGenerationActive(audio_worker_generation_)) break;
            if (action.type == AudioReorderAction::Type::Missing) {
                if (last_decoded_audio_end_ns_ == 0) {
                    if (auto* perf = perfStats()) perf->recordAudioDrop();
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
    if (packet.data.empty() ||
        !boundedVideoPacketIsCurrent(running_.load(), packet.generation,
                                     video_worker_generation_,
                                     packet.recovery_epoch,
                                     video_recovery_epoch_.load()) ||
        !isGenerationActive(packet.generation)) {
        return;
    }
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    auto* perf = perfStats();
#endif
    if (!boundedVideoMayDecodeWhileRecovering(
            video_waiting_for_keyframe_.load(), packet.contains_idr)) {
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
#if LUNARNX_DROP_DIAGNOSTIC_LOG
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
        const bool rendered = video_renderer_->render(frame);
        if (lunar::shouldSampleCloud1080CrashProbe(probe_frame_index)) {
            lunar::cloud1080CrashProbeLog(
                "crash-probe",
                "DEBUG-c1080 phase=render-queued frame=%llu result=%d",
                static_cast<unsigned long long>(probe_frame_index),
                rendered ? 1 : 0);
        }
        if (rendered && perf) perf->recordFrame();
        if (rendered && !video_ready_notified_.exchange(true)) {
            std::lock_guard<std::mutex> lock(video_ready_callback_mutex_);
            if (video_ready_callback_) video_ready_callback_();
        }
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

bool MediaPipeline::submitDecodedAudio(const AudioFrame& frame,
                                       uint32_t generation) {
    if (!isGenerationActive(generation) || !audio_player_ || !av_sync_) return false;
    if (!audio_player_->play(frame)) return false;
    if (auto* perf = perfStats()) perf->recordAudioFrame();
    const uint64_t playback_ts = estimateAudioPlaybackTimestamp(
        frame.timestamp, frame.sample_count, frame.sample_rate,
        audio_player_->queuedSampleCount());
    av_sync_->updateAudioPts(playback_ts);
    return true;
}

void MediaPipeline::presentVideoFrame() {
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    if (running_.load() && video_renderer_) {
        video_renderer_->present();
    }
}

} // namespace lunar::stream
