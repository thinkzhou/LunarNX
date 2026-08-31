#pragma once

#ifdef __SWITCH__

#include <chiaki/session.h>
#include <chiaki/audio.h>
#include <chiaki/opusdecoder.h>
#include "ps_connection_trace.h"
#include "../stream/media_pipeline.h"
#include "../stream/audio_decoder.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <vector>

namespace lunar::ps {

class PsMediaBridge {
public:
    using EventCallback = std::function<void(ChiakiEvent*)>;
    using RumbleCallback = std::function<void(uint8_t left, uint8_t right)>;
    explicit PsMediaBridge(stream::MediaPipeline& media, int fps,
                           std::shared_ptr<PsConnectionTrace> trace = {});
    ~PsMediaBridge();

    PsMediaBridge(const PsMediaBridge&) = delete;
    PsMediaBridge& operator=(const PsMediaBridge&) = delete;

    // Chiaki callbacks (called from chiaki's internal thread)
    bool onVideoSample(uint8_t* data, size_t size, int32_t frames_lost, bool recovered);
    // Chiaki can deliver the first access units before the asynchronously
    // initialized MediaPipeline is running. Publish the bounded startup
    // buffer after initialize() completes, before requesting a fresh IDR.
    void setMediaReady();

    ChiakiAudioSink audioSink();
    void initializeAudio(ChiakiLog* log);
    ChiakiEventCallback eventCallback();

    void setEventForwarder(EventCallback cb) { event_cb_ = std::move(cb); }
    void setRumbleForwarder(RumbleCallback cb) { rumble_cb_ = std::move(cb); }

    // Static C callbacks exposed for chiaki API registration
    static bool videoSampleCb(uint8_t* buf, size_t buf_size, int32_t frames_lost,
                              bool frame_recovered, void* user);
    static void eventCb(ChiakiEvent* event, void* user);

private:
    struct PendingVideoSample {
        std::vector<uint8_t> data;
        int32_t frames_lost = 0;
        uint64_t pts = 0;
    };

    stream::MediaPipeline& media_;
    int fps_;
    std::shared_ptr<PsConnectionTrace> trace_;

    std::mutex video_mutex_;
    bool media_ready_ = false;
    std::deque<PendingVideoSample> pending_video_samples_;
    size_t pending_video_bytes_ = 0;
    bool first_video_sample_logged_ = false;

    // Video PTS tracking
    uint64_t video_frame_count_ = 0;
    std::atomic<uint64_t> next_video_pts_ns_{0};

    // Audio clock
    int audio_sample_rate_ = 48000;
    int audio_channels_ = 2;
    int audio_frame_size_ = 0;
    uint64_t audio_samples_played_ = 0;
    uint64_t audio_epoch_ns_ = 0;

    EventCallback event_cb_;
    RumbleCallback rumble_cb_;
    ChiakiOpusDecoder opus_decoder_{};
    ChiakiAudioSink opus_sink_{};
    bool opus_decoder_initialized_ = false;
    bool audio_format_valid_ = false;
    std::atomic<bool> first_audio_header_logged_{false};
    std::atomic<bool> first_audio_packet_logged_{false};
    std::atomic<bool> first_decoded_audio_logged_{false};

    // Internal audio frame callback (called from chiaki thread)
    void onDecodedAudioFrame(int16_t* buf, size_t samples_count);

    static void audioHeaderCb(ChiakiAudioHeader* header, void* user);
    static void audioFrameCb(uint8_t* buf, size_t buf_size, void* user);
    static void decodedAudioSettingsCb(uint32_t channels, uint32_t rate, void* user);
    static void decodedAudioFrameCb(int16_t* buf, size_t samples_count, void* user);
};

} // namespace lunar::ps

#endif
