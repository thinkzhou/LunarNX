#pragma once

#include "../common.h"
#include "audio_decoder.h"
#include "perf_stats.h"
#include <array>
#include <cstddef>
#include <mutex>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace lunar::stream {

/// Audio output: libnx audout (Switch), SDL2 (Desktop)
/// Uses ring buffer for thread-safe async playback.
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    bool initialize(int sample_rate = 48000, int channels = 2);
    bool play(const AudioFrame& frame);
    // Drop queued samples without tearing down the audio device. This is used
    // when a new WebRTC association becomes the active media source.
    void flush();
    size_t queuedSampleCount();
    void setPerfStats(PerfStats* stats) { perf_ = stats; }
    void setVolume(float volume);
    void shutdown();

private:
    int sample_rate_ = 48000;
    int channels_ = 2;
    float volume_ = 1.0f;
    bool initialized_ = false;
    PerfStats* perf_ = nullptr;

    std::mutex mutex_;

#ifdef __SWITCH__
    static constexpr size_t BUFFER_COUNT = 5;

    int freeWavebufIndex() const;
    uint32_t queuedWavebufCount() const;
    void recordAudioLatencyStats(size_t queued_samples);
    size_t appendAudio(const void* data, size_t size);
    bool writeAudio(const void* data, size_t size);

    AudioDriver driver_{};
    std::array<AudioDriverWaveBuf, BUFFER_COUNT> wavebufs_{};
    AudioDriverWaveBuf* current_wavebuf_ = nullptr;
    Mutex update_lock_{};
    void* mempool_ = nullptr;
    void* current_pool_ = nullptr;
    size_t mempool_size_ = 0;
    size_t buffer_size_ = 0;
    size_t samples_per_buffer_ = 0;
    size_t current_size_ = 0;
    size_t total_queued_samples_ = 0;
    bool wavebuf_enqueue_failed_ = false;
    bool audren_initialized_ = false;
    bool driver_initialized_ = false;
#else
    uint32_t audio_dev_ = 0;
#endif
};

} // namespace lunar::stream
