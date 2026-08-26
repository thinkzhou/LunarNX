#include "audio_player.h"
#include "../diagnostics.h"
#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdint>

#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>
#else
#include <SDL2/SDL.h>
#endif

namespace lunar::stream {

#ifdef __SWITCH__
namespace {

constexpr int kAudrenVoiceId = 0;
constexpr uint32_t kAudioOverflowMs = 500;
constexpr uint8_t kSinkChannels[] = {0, 1};
constexpr AudioRendererConfig kAudrenConfig{
    AudioRendererOutputRate_48kHz,
    24,
    0,
    1,
    1,
    2,
};
constexpr int kAudioPlayerLogLimit = 3;
constexpr int kAudioPlayerDetailLogLimit = 24;
std::atomic<int> g_audio_player_logs{0};
std::atomic<int> g_audio_player_detail_logs{0};

size_t alignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void applyVolume(int16_t* samples, size_t sample_count, float volume) {
    if (volume >= 0.999f) return;
    if (volume < 0.0f) volume = 0.0f;
    for (size_t i = 0; i < sample_count; i++) {
        int scaled = static_cast<int>(samples[i] * volume);
        samples[i] = static_cast<int16_t>(std::min(SHRT_MAX, std::max(SHRT_MIN, scaled)));
    }
}

bool shouldLogAudioPlayer(int index) {
    return index < kAudioPlayerLogLimit;
}

void logAudioPlayerDetail(const char* format, ...) {
    if (g_audio_player_detail_logs.fetch_add(1) >= kAudioPlayerDetailLogLimit) return;

    char message[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    lunar::diagnosticLog("audio-player", "%s", message);
}

} // namespace
#endif

AudioPlayer::AudioPlayer() = default;
AudioPlayer::~AudioPlayer() { shutdown(); }

bool AudioPlayer::initialize(int sample_rate, int channels,
                             AudioLatencyMode latency_mode) {
    sample_rate_ = sample_rate;
    channels_ = channels;
    latency_mode_ = latency_mode;
    requested_latency_mode_ = latency_mode;

#ifdef __SWITCH__
    g_audio_player_logs = 0;
    g_audio_player_detail_logs = 0;
    const auto buffer_config = audioBufferConfig(latency_mode);
    active_buffer_count_ = buffer_config.buffer_count;
    audren_frames_per_buffer_ = buffer_config.audren_frames_per_buffer;
    lunar::diagnosticLog("audio-player", "initialize begin sample_rate=%d channels=%d mode=%s frames_per_buffer=%zu buffers=%zu",
                         sample_rate,
                         channels,
                         audioLatencyModeName(latency_mode),
                         audren_frames_per_buffer_,
                         active_buffer_count_);
    if (sample_rate != 48000 || channels != 2) {
        fprintf(stderr, "[audio] audren only supports 48kHz stereo PCM for now\n");
        lunar::diagnosticLog("audio-player", "unsupported format");
        return false;
    }

    lunar::diagnosticLog("audio-player", "mempool alloc begin");
    mutexInit(&update_lock_);
    samples_per_buffer_ = static_cast<size_t>(AUDREN_SAMPLES_PER_FRAME_48KHZ);
    buffer_size_ = samples_per_buffer_ * static_cast<size_t>(channels) * sizeof(int16_t);
    buffer_size_ *= audren_frames_per_buffer_;
    samples_per_buffer_ *= audren_frames_per_buffer_;
    // Allocate the full five-buffer geometry once. Cloud Balanced and
    // Recovery share the same 25 ms wave-buffer size and can then move between
    // four and five active buffers without rebuilding Audren mid-stream.
    mempool_size_ = alignUp(buffer_size_ * MAX_BUFFER_COUNT,
                            AUDREN_MEMPOOL_ALIGNMENT);
    mempool_ = memalign(AUDREN_MEMPOOL_ALIGNMENT, mempool_size_);
    if (!mempool_) {
        fprintf(stderr, "[audio] audren mempool alloc failed\n");
        lunar::diagnosticLog("audio-player", "mempool alloc failed size=%zu",
                             mempool_size_);
        return false;
    }
    lunar::diagnosticLog("audio-player", "mempool alloc done size=%zu",
                         mempool_size_);
    std::memset(mempool_, 0, mempool_size_);
    std::memset(&driver_, 0, sizeof(driver_));
    for (auto& wavebuf : wavebufs_) wavebuf = {};

    lunar::diagnosticLog("audio-player", "audrenInitialize begin");
    Result rc = audrenInitialize(&kAudrenConfig);
    if (R_FAILED(rc)) {
        fprintf(stderr, "[audio] audrenInitialize failed 0x%x\n", rc);
        lunar::diagnosticLog("audio-player", "audrenInitialize failed rc=0x%x", rc);
        shutdown();
        return false;
    }
    audren_initialized_ = true;
    lunar::diagnosticLog("audio-player", "audrenInitialize done");

    lunar::diagnosticLog("audio-player", "audrvCreate begin");
    rc = audrvCreate(&driver_, &kAudrenConfig, channels);
    if (R_FAILED(rc)) {
        fprintf(stderr, "[audio] audrvCreate failed 0x%x\n", rc);
        lunar::diagnosticLog("audio-player", "audrvCreate failed rc=0x%x", rc);
        shutdown();
        return false;
    }
    driver_initialized_ = true;
    lunar::diagnosticLog("audio-player", "audrvCreate done");

    for (size_t i = 0; i < MAX_BUFFER_COUNT; i++) {
        wavebufs_[i].data_raw = mempool_;
        wavebufs_[i].size = mempool_size_;
        wavebufs_[i].start_sample_offset = static_cast<u32>(i * samples_per_buffer_);
        wavebufs_[i].end_sample_offset = static_cast<u32>((i + 1) * samples_per_buffer_);
    }

    int mempool_id = audrvMemPoolAdd(&driver_, mempool_, mempool_size_);
    audrvMemPoolAttach(&driver_, mempool_id);
    audrvDeviceSinkAdd(&driver_, AUDREN_DEFAULT_DEVICE_NAME, channels, kSinkChannels);

    lunar::diagnosticLog("audio-player", "audrenStartAudioRenderer begin");
    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc)) {
        fprintf(stderr, "[audio] audrenStartAudioRenderer failed 0x%x\n", rc);
        lunar::diagnosticLog("audio-player",
                             "audrenStartAudioRenderer failed rc=0x%x",
                             rc);
        shutdown();
        return false;
    }
    lunar::diagnosticLog("audio-player", "audrenStartAudioRenderer done");

    lunar::diagnosticLog("audio-player", "voice init begin");
    audrvVoiceInit(&driver_, kAudrenVoiceId, channels, PcmFormat_Int16, sample_rate);
    audrvVoiceSetDestinationMix(&driver_, kAudrenVoiceId, AUDREN_FINAL_MIX_ID);
    for (int in = 0; in < channels; in++) {
        for (int out = 0; out < channels; out++) {
            audrvVoiceSetMixFactor(&driver_, kAudrenVoiceId, in == out ? 1.0f : 0.0f, in, out);
        }
    }
    audrvVoiceStart(&driver_, kAudrenVoiceId);
    audrvUpdate(&driver_);
    lunar::diagnosticLog("audio-player", "voice init done");

    initialized_ = true;
    if (perf_) perf_->recordAudioLatency(
        0,
        static_cast<uint32_t>((samples_per_buffer_ * 1000) / static_cast<size_t>(sample_rate_)),
        kAudioOverflowMs);
    fprintf(stderr,"[audio] audren %dHz %dch\n", sample_rate, channels);
    lunar::diagnosticLog("audio-player", "initialize done");
    return true;
#else
    if (SDL_Init(SDL_INIT_AUDIO) < 0) return false;
    SDL_AudioSpec want={},have={};
    want.freq=sample_rate; want.format=AUDIO_S16SYS; want.channels=channels; want.samples=1024;
    audio_dev_=SDL_OpenAudioDevice(nullptr,0,&want,&have,0);
    if(!audio_dev_)return false;
    SDL_PauseAudioDevice(audio_dev_,0);
    initialized_=true;
    return true;
#endif
}

bool AudioPlayer::setLatencyMode(AudioLatencyMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef __SWITCH__
    const auto config = audioBufferConfig(mode);
    if (initialized_ &&
        config.audren_frames_per_buffer != audren_frames_per_buffer_) {
        // Reinterpreting live wave-buffer offsets would corrupt playback.
        // Xbox cloud only switches Balanced <-> Resilient, which both use
        // 25 ms buffers; home Realtime remains fixed at 20 ms.
        return false;
    }
    requested_latency_mode_ = mode;
    const bool applied = tryApplyRequestedLatencyModeLocked();
#else
    requested_latency_mode_ = mode;
    latency_mode_ = mode;
#endif
    lunar::diagnosticLog("audio-player",
                         "latency mode request=%s active=%s capacity_ms=%zu%s",
                         audioLatencyModeName(mode),
                         audioLatencyModeName(latency_mode_),
                         audioBufferCapacityMs(latency_mode_)
#ifdef __SWITCH__
                         , applied ? "" : " deferred-until-drained"
#else
                         , ""
#endif
                         );
    return true;
}

bool AudioPlayer::play(const AudioFrame& frame) {
    if (!initialized_) return false;

#ifdef __SWITCH__
    const int log_index = g_audio_player_logs.fetch_add(1);
    const bool log = shouldLogAudioPlayer(log_index);
    if (log) {
        lunar::diagnosticLog("audio-player",
                             "play begin index=%d pcm=%zu samples=%zu rate=%d channels=%d",
                             log_index,
                             frame.pcm_data.size(),
                             frame.sample_count,
                             frame.sample_rate,
                             frame.channels);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (log) lunar::diagnosticLog("audio-player", "play lock acquired");
    bool ok = writeAudio(frame.pcm_data.data(), frame.pcm_data.size());
    if (log) {
        lunar::diagnosticLog("audio-player", "writeAudio returned %s", ok ? "true" : "false");
    }
    if (perf_) perf_->recordAudioQueuedBuffers(queuedWavebufCount());
    if (!ok && perf_) perf_->recordAudioDrop();
    if (log) lunar::diagnosticLog("audio-player", "play done index=%d", log_index);
    return ok;
#else
    if (!audio_dev_) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    int q = SDL_GetQueuedAudioSize(audio_dev_);
    if (q > sample_rate_ * channels_ * 2) {
        SDL_ClearQueuedAudio(audio_dev_);
        if (perf_) perf_->recordAudioDrop();
    }
    SDL_QueueAudio(audio_dev_, frame.pcm_data.data(), (Uint32)frame.pcm_data.size());
    if (perf_) perf_->recordAudioQueuedBuffers(SDL_GetQueuedAudioSize(audio_dev_));
    return true;
#endif
}

void AudioPlayer::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef __SWITCH__
    if (!driver_initialized_) return;

    audrvVoiceStop(&driver_, kAudrenVoiceId);
    mutexLock(&update_lock_);
    audrvUpdate(&driver_);
    mutexUnlock(&update_lock_);
    // Stop and drain the voice, but keep each descriptor's data pointer,
    // capacity, and sample offsets intact so the next source can reuse the
    // existing Audren voice and memory pool.
    for (auto& wavebuf : wavebufs_) {
        wavebuf.state = AudioDriverWaveBufState_Free;
        wavebuf.next = nullptr;
        wavebuf.sequence_id = UINT32_MAX;
    }
    current_wavebuf_ = nullptr;
    current_pool_ = nullptr;
    current_size_ = 0;
    total_queued_samples_ = 0;
    wavebuf_enqueue_failed_ = false;
#else
    if (audio_dev_) SDL_ClearQueuedAudio(audio_dev_);
#endif
    if (perf_) perf_->recordAudioQueuedBuffers(0);
}

size_t AudioPlayer::queuedSampleCount() {
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef __SWITCH__
    if (!driver_initialized_) return 0;
    mutexLock(&update_lock_);
    audrvUpdate(&driver_);
    mutexUnlock(&update_lock_);
    const size_t played_samples =
        audrvVoiceGetPlayedSampleCount(&driver_, kAudrenVoiceId);
    return total_queued_samples_ > played_samples
        ? total_queued_samples_ - played_samples
        : 0;
#else
    if (!audio_dev_ || channels_ <= 0) return 0;
    const size_t bytes = SDL_GetQueuedAudioSize(audio_dev_);
    return bytes / (static_cast<size_t>(channels_) * sizeof(int16_t));
#endif
}

#ifdef __SWITCH__
int AudioPlayer::freeWavebufIndex() const {
    for (size_t i = 0; i < active_buffer_count_; i++) {
        auto state = wavebufs_[i].state;
        if (state == AudioDriverWaveBufState_Free ||
            state == AudioDriverWaveBufState_Done) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint32_t AudioPlayer::queuedWavebufCount() const {
    uint32_t count = 0;
    for (size_t i = 0; i < active_buffer_count_; i++) {
        const auto& wavebuf = wavebufs_[i];
        if (wavebuf.state == AudioDriverWaveBufState_Queued ||
            wavebuf.state == AudioDriverWaveBufState_Playing) {
            count++;
        }
    }
    if (current_wavebuf_) count++;
    return count;
}

bool AudioPlayer::tryApplyRequestedLatencyModeLocked() {
    const auto requested = audioBufferConfig(requested_latency_mode_);
    const size_t requested_count =
        std::min(requested.buffer_count, MAX_BUFFER_COUNT);
    if (requested_count < active_buffer_count_) {
        for (size_t i = requested_count; i < active_buffer_count_; ++i) {
            const auto state = wavebufs_[i].state;
            if (&wavebufs_[i] == current_wavebuf_ ||
                state == AudioDriverWaveBufState_Queued ||
                state == AudioDriverWaveBufState_Playing) {
                return false;
            }
        }
    }
    active_buffer_count_ = requested_count;
    latency_mode_ = requested_latency_mode_;
    return true;
}

void AudioPlayer::recordAudioLatencyStats(size_t queued_samples) {
    if (!perf_ || sample_rate_ <= 0) return;
    uint32_t latency_ms = static_cast<uint32_t>(
        (queued_samples * 1000) / static_cast<size_t>(sample_rate_));
    uint32_t buffer_ms = static_cast<uint32_t>(
        (samples_per_buffer_ * 1000) / static_cast<size_t>(sample_rate_));
    perf_->recordAudioLatency(latency_ms, buffer_ms, kAudioOverflowMs);
}

size_t AudioPlayer::appendAudio(const void* data, size_t size) {
    if (!current_wavebuf_) {
        int index = freeWavebufIndex();
        if (index < 0) return 0;
        current_wavebuf_ = &wavebufs_[static_cast<size_t>(index)];
        current_pool_ = static_cast<uint8_t*>(mempool_) +
            static_cast<size_t>(index) * buffer_size_;
        current_size_ = 0;
        logAudioPlayerDetail("append select wavebuf=%d buffer_size=%zu samples_per_buffer=%zu",
                             index,
                             buffer_size_,
                             samples_per_buffer_);
    }

    size_t writable = std::min(size, buffer_size_ - current_size_);
    void* dst = static_cast<uint8_t*>(current_pool_) + current_size_;
    logAudioPlayerDetail("append copy size=%zu writable=%zu current_size=%zu",
                         size,
                         writable,
                         current_size_);
    std::memcpy(dst, data, writable);
    applyVolume(static_cast<int16_t*>(dst), writable / sizeof(int16_t), volume_);
    armDCacheFlush(dst, writable);
    logAudioPlayerDetail("append copied/flushed writable=%zu", writable);

    current_size_ += writable;
    total_queued_samples_ += writable / (static_cast<size_t>(channels_) * sizeof(int16_t));

    if (current_size_ == buffer_size_) {
        logAudioPlayerDetail("append queue wavebuf begin");
        if (!audrvVoiceAddWaveBuf(&driver_, kAudrenVoiceId, current_wavebuf_)) {
            lunar::persistentEventLog("audio-player", "wave-buffer enqueue failed");
            const size_t buffer_samples = current_size_ /
                (static_cast<size_t>(channels_) * sizeof(int16_t));
            if (total_queued_samples_ >= buffer_samples) {
                total_queued_samples_ -= buffer_samples;
            } else {
                total_queued_samples_ = 0;
            }
            current_wavebuf_->state = AudioDriverWaveBufState_Free;
            current_wavebuf_->next = nullptr;
            current_wavebuf_->sequence_id = UINT32_MAX;
            current_wavebuf_ = nullptr;
            current_pool_ = nullptr;
            current_size_ = 0;
            wavebuf_enqueue_failed_ = true;
            return 0;
        }
        mutexLock(&update_lock_);
        audrvUpdate(&driver_);
        mutexUnlock(&update_lock_);
        logAudioPlayerDetail("append audrvUpdate done");
        if (!audrvVoiceIsPlaying(&driver_, kAudrenVoiceId)) {
            audrvVoiceStart(&driver_, kAudrenVoiceId);
        }
        current_wavebuf_ = nullptr;
        current_pool_ = nullptr;
        current_size_ = 0;
    }

    return writable;
}

bool AudioPlayer::writeAudio(const void* data, size_t size) {
    if (!driver_initialized_ || !data || size == 0) return false;

    wavebuf_enqueue_failed_ = false;

    logAudioPlayerDetail("write begin size=%zu", size);
    mutexLock(&update_lock_);
    audrvUpdate(&driver_);
    mutexUnlock(&update_lock_);
    // Expanding the ring is immediate. A recovery -> balanced downshift waits
    // until the fifth descriptor has naturally drained, so lowering latency
    // never discards already queued PCM.
    tryApplyRequestedLatencyModeLocked();
    logAudioPlayerDetail("write initial audrvUpdate done");

    size_t played_samples = audrvVoiceGetPlayedSampleCount(&driver_, kAudrenVoiceId);
    size_t queued_samples = total_queued_samples_ > played_samples
        ? total_queued_samples_ - played_samples
        : 0;
    logAudioPlayerDetail("write queued played=%zu total=%zu queued=%zu",
                         played_samples,
                         total_queued_samples_,
                         queued_samples);
    recordAudioLatencyStats(queued_samples);
    const size_t overflow_samples =
        (static_cast<size_t>(sample_rate_) * kAudioOverflowMs) / 1000;
    if (queued_samples > overflow_samples) {
        return false;
    }

    size_t written = 0;
    size_t consecutive_waits = 0;
    while (written < size) {
        logAudioPlayerDetail("write loop written=%zu size=%zu", written, size);
        size_t step = appendAudio(static_cast<const uint8_t*>(data) + written,
                                  size - written);
        logAudioPlayerDetail("write append step=%zu", step);
        if (step == 0) {
            if (wavebuf_enqueue_failed_) return false;
            mutexLock(&update_lock_);
            audrvUpdate(&driver_);
            mutexUnlock(&update_lock_);
            if (freeWavebufIndex() >= 0) {
                consecutive_waits = 0;
                continue;
            }
            if (consecutive_waits >= audren_frames_per_buffer_) return false;
            logAudioPlayerDetail("write wait frame begin");
            audrenWaitFrame();
            logAudioPlayerDetail("write wait frame done");
            consecutive_waits++;
            continue;
        }
        consecutive_waits = 0;
        written += step;
    }

    size_t final_played_samples = audrvVoiceGetPlayedSampleCount(&driver_, kAudrenVoiceId);
    size_t final_queued_samples = total_queued_samples_ > final_played_samples
        ? total_queued_samples_ - final_played_samples
        : 0;
    recordAudioLatencyStats(final_queued_samples);
    logAudioPlayerDetail("write done played=%zu queued=%zu",
                         final_played_samples,
                         final_queued_samples);
    return true;
}
#endif

void AudioPlayer::setVolume(float volume) { volume_ = volume; }

void AudioPlayer::shutdown() {
#ifdef __SWITCH__
    if (driver_initialized_) {
        audrvVoiceStop(&driver_, kAudrenVoiceId);
        audrvClose(&driver_);
        driver_initialized_ = false;
    }
    if (audren_initialized_) {
        audrenExit();
        audren_initialized_ = false;
    }
    if (mempool_) {
        free(mempool_);
        mempool_ = nullptr;
    }
    for (auto& wavebuf : wavebufs_) wavebuf = {};
    current_wavebuf_ = nullptr;
    current_pool_ = nullptr;
    mempool_size_ = 0;
    buffer_size_ = 0;
    samples_per_buffer_ = 0;
    active_buffer_count_ = MAX_BUFFER_COUNT;
    audren_frames_per_buffer_ = 5;
    current_size_ = 0;
    total_queued_samples_ = 0;
#else
    if (audio_dev_) { SDL_CloseAudioDevice(audio_dev_); audio_dev_=0; }
#endif
    initialized_ = false;
    latency_mode_ = AudioLatencyMode::Resilient;
    requested_latency_mode_ = AudioLatencyMode::Resilient;
    if (perf_) perf_->recordAudioQueuedBuffers(0);
}

} // namespace lunar::stream
