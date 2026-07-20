#include "audio_player.h"
#include "../diagnostics.h"
#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <climits>
#include <cstdio>
#include <cstring>

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
constexpr int kAudioLatencyFrames = 5;
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
constexpr int kAudioPlayerLogLimit = 12;
constexpr int kAudioPlayerDetailLogLimit = 160;
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

bool AudioPlayer::initialize(int sample_rate, int channels) {
    sample_rate_ = sample_rate;
    channels_ = channels;

#ifdef __SWITCH__
    g_audio_player_logs = 0;
    g_audio_player_detail_logs = 0;
    lunar::diagnosticLog("audio-player", "initialize begin sample_rate=%d channels=%d",
                         sample_rate,
                         channels);
    if (sample_rate != 48000 || channels != 2) {
        fprintf(stderr, "[audio] audren only supports 48kHz stereo PCM for now\n");
        lunar::diagnosticLog("audio-player", "unsupported format");
        return false;
    }

    lunar::diagnosticLog("audio-player", "mempool alloc begin");
    mutexInit(&update_lock_);
    samples_per_buffer_ = static_cast<size_t>(AUDREN_SAMPLES_PER_FRAME_48KHZ);
    buffer_size_ = samples_per_buffer_ * static_cast<size_t>(channels) * sizeof(int16_t);
    buffer_size_ *= kAudioLatencyFrames;
    samples_per_buffer_ *= kAudioLatencyFrames;
    mempool_size_ = alignUp(buffer_size_ * BUFFER_COUNT, AUDREN_MEMPOOL_ALIGNMENT);
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

    for (size_t i = 0; i < BUFFER_COUNT; i++) {
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
    for (size_t i = 0; i < wavebufs_.size(); i++) {
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
    for (const auto& wavebuf : wavebufs_) {
        if (wavebuf.state == AudioDriverWaveBufState_Queued ||
            wavebuf.state == AudioDriverWaveBufState_Playing) {
            count++;
        }
    }
    if (current_wavebuf_) count++;
    return count;
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
        audrvVoiceAddWaveBuf(&driver_, kAudrenVoiceId, current_wavebuf_);
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

    logAudioPlayerDetail("write begin size=%zu", size);
    mutexLock(&update_lock_);
    audrvUpdate(&driver_);
    mutexUnlock(&update_lock_);
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
    while (written < size) {
        logAudioPlayerDetail("write loop written=%zu size=%zu", written, size);
        size_t step = appendAudio(static_cast<const uint8_t*>(data) + written,
                                  size - written);
        logAudioPlayerDetail("write append step=%zu", step);
        if (step == 0) {
            mutexLock(&update_lock_);
            audrvUpdate(&driver_);
            mutexUnlock(&update_lock_);
            logAudioPlayerDetail("write wait frame begin");
            audrenWaitFrame();
            logAudioPlayerDetail("write wait frame done");
            if (freeWavebufIndex() < 0) return false;
            continue;
        }
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
    current_size_ = 0;
    total_queued_samples_ = 0;
#else
    if (audio_dev_) { SDL_CloseAudioDevice(audio_dev_); audio_dev_=0; }
#endif
    initialized_ = false;
    if (perf_) perf_->recordAudioQueuedBuffers(0);
}

} // namespace lunar::stream
