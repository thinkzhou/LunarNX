#ifdef __SWITCH__

#include "ps_media_bridge.h"
#include "../diagnostics.h"
#include <cstring>
#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace lunar::ps {

namespace {
constexpr size_t kMaxStartupVideoBytes = 8 * 1024 * 1024;
}

PsMediaBridge::PsMediaBridge(
    stream::MediaPipeline& media, int fps,
    std::shared_ptr<PsConnectionTrace> trace)
    : media_(media), fps_(std::clamp(fps, 1, 120)),
      trace_(std::move(trace)) {}

PsMediaBridge::~PsMediaBridge() {
    if (opus_decoder_initialized_) chiaki_opus_decoder_fini(&opus_decoder_);
}

bool PsMediaBridge::onVideoSample(uint8_t* data, size_t size, int32_t frames_lost, bool recovered) {
    std::lock_guard<std::mutex> lock(video_mutex_);
    if (!first_video_sample_logged_) {
        first_video_sample_logged_ = true;
        if (trace_) trace_->record(
            "first-video-sample", data && size > 0 ? "received" : "invalid",
            "bytes=%zu frames_lost=%d recovered=%d media_ready=%d",
            size, frames_lost, recovered ? 1 : 0, media_ready_ ? 1 : 0);
    }

    // Calculate PTS
    if (video_frame_count_ == 0) {
        next_video_pts_ns_.store(0);
    }
    const uint64_t pts = next_video_pts_ns_.load();

    // Advance PTS: (frames_lost + 1) frames at nominal duration
    const uint64_t frame_duration_ns = 1000000000ULL / static_cast<uint64_t>(fps_);
    const uint64_t frame_advance = static_cast<uint64_t>(std::clamp(frames_lost, 0, 120) + 1);
    if (frame_advance <= (std::numeric_limits<uint64_t>::max() - pts) /
                             frame_duration_ns) {
        next_video_pts_ns_.store(pts + frame_advance * frame_duration_ns);
    }
    video_frame_count_++;

    if (!media_ready_) {
        // Chiaki owns and reuses its callback buffer, so startup samples must
        // be copied before returning. Keep only a bounded suffix; the IDR
        // requested after initialization remains the authoritative recovery
        // path if the startup burst exceeds the cap.
        if (!data || size == 0 || size > kMaxStartupVideoBytes) return true;
        try {
            while (!pending_video_samples_.empty() &&
                   pending_video_bytes_ + size > kMaxStartupVideoBytes) {
                pending_video_bytes_ -= pending_video_samples_.front().data.size();
                pending_video_samples_.pop_front();
            }
            PendingVideoSample sample;
            sample.data.assign(data, data + size);
            sample.frames_lost = frames_lost;
            sample.pts = pts;
            pending_video_bytes_ += sample.data.size();
            pending_video_samples_.push_back(std::move(sample));
        } catch (...) {
            // A startup allocation failure must not make Chiaki tear down the
            // session. The post-init IDR request can still recover the stream.
        }
        return true;
    }

    media_.recordIncomingVideoSample(size, pts,
        static_cast<uint32_t>(std::max(frames_lost, 0)));
    // Copy data - chiaki reuses the buffer after callback returns
    const bool queued = media_.decodeVideoPacket(data, size, pts);
    (void)recovered;
    return queued;
}

void PsMediaBridge::setMediaReady() {
    std::lock_guard<std::mutex> lock(video_mutex_);
    if (media_ready_) return;
    if (trace_) trace_->record(
        "startup-video-buffer", "flush",
        "samples=%zu bytes=%zu", pending_video_samples_.size(),
        pending_video_bytes_);
    media_ready_ = true;

    for (auto& sample : pending_video_samples_) {
        media_.recordIncomingVideoSample(
            sample.data.size(), sample.pts,
            static_cast<uint32_t>(std::max(sample.frames_lost, 0)));
        (void)media_.decodeVideoPacket(sample.data.data(), sample.data.size(),
                                       sample.pts);
    }
    pending_video_samples_.clear();
    pending_video_bytes_ = 0;
}

void PsMediaBridge::initializeAudio(ChiakiLog* log) {
    if (opus_decoder_initialized_) return;
    chiaki_opus_decoder_init(&opus_decoder_, log);
    chiaki_opus_decoder_set_cb(&opus_decoder_, decodedAudioSettingsCb,
                               decodedAudioFrameCb, this);
    chiaki_opus_decoder_get_sink(&opus_decoder_, &opus_sink_);
    opus_decoder_initialized_ = true;
}

ChiakiAudioSink PsMediaBridge::audioSink() {
    ChiakiAudioSink sink{};
    sink.user = this;
    sink.header_cb = audioHeaderCb;
    sink.frame_cb = audioFrameCb;
    return sink;
}

ChiakiEventCallback PsMediaBridge::eventCallback() {
    return eventCb;
}

void PsMediaBridge::onDecodedAudioFrame(int16_t* buf, size_t sample_count) {
    if (!first_decoded_audio_logged_.exchange(true) && trace_) {
        trace_->record(
            "first-decoded-audio", buf && sample_count > 0 ? "received" : "invalid",
            "samples=%zu format_valid=%d frame_size=%d",
            sample_count, audio_format_valid_ ? 1 : 0, audio_frame_size_);
    }
    if (!audio_format_valid_ || !buf || sample_count == 0 ||
        sample_count > static_cast<size_t>(audio_frame_size_)) return;

    // Calculate audio timestamp
    uint64_t pts = 0;
    if (audio_epoch_ns_ == 0 && audio_sample_rate_ > 0) {
        audio_epoch_ns_ = next_video_pts_ns_.load();
    }
    pts = audio_epoch_ns_ + (audio_samples_played_ * 1000000000ULL) /
          static_cast<uint64_t>(audio_sample_rate_);
    audio_samples_played_ += sample_count;

    // Build AudioFrame and feed to MediaPipeline
    stream::AudioFrame frame;
    const size_t pcm_samples = sample_count * static_cast<size_t>(audio_channels_);
    const auto* bytes = reinterpret_cast<const uint8_t*>(buf);
    frame.pcm_data.assign(bytes, bytes + pcm_samples * sizeof(int16_t));
    frame.sample_count = sample_count;
    frame.sample_rate = audio_sample_rate_;
    frame.channels = audio_channels_;
    frame.timestamp = pts;

    media_.playDecodedAudio(frame);
}

// Static C callbacks
bool PsMediaBridge::videoSampleCb(uint8_t* buf, size_t buf_size, int32_t frames_lost,
                                   bool frame_recovered, void* user) {
    auto* self = static_cast<PsMediaBridge*>(user);
    if (!self) return false;
    return self->onVideoSample(buf, buf_size, frames_lost, frame_recovered);
}

void PsMediaBridge::audioHeaderCb(ChiakiAudioHeader* header, void* user) {
    auto* self = static_cast<PsMediaBridge*>(user);
    if (!self || !header) return;
    self->audio_format_valid_ = header->rate == 48000 && header->channels == 2 &&
        header->bits == 16 && header->frame_size > 0 && header->frame_size <= 5760;
    if (!self->first_audio_header_logged_.exchange(true) && self->trace_) {
        self->trace_->record(
            "audio-header", self->audio_format_valid_ ? "valid" : "invalid",
            "rate=%u channels=%u bits=%u frame_size=%u sink_ready=%d",
            header->rate, static_cast<unsigned int>(header->channels),
            static_cast<unsigned int>(header->bits), header->frame_size,
            self->opus_sink_.header_cb ? 1 : 0);
    }
    if (!self->audio_format_valid_ || !self->opus_sink_.header_cb) return;
    self->audio_sample_rate_ = header->rate;
    self->audio_channels_ = header->channels;
    self->audio_frame_size_ = header->frame_size;
    self->audio_samples_played_ = 0;
    self->audio_epoch_ns_ = 0;
    self->opus_sink_.header_cb(header, self->opus_sink_.user);
}

void PsMediaBridge::audioFrameCb(uint8_t* buf, size_t buf_size, void* user) {
    auto* self = static_cast<PsMediaBridge*>(user);
    if (!self) return;
    if (!self->first_audio_packet_logged_.exchange(true) && self->trace_) {
        self->trace_->record(
            "first-audio-packet",
            buf && buf_size > 0 && self->audio_format_valid_ &&
                    self->opus_sink_.frame_cb
                ? "received" : "invalid",
            "bytes=%zu format_valid=%d sink_ready=%d",
            buf_size, self->audio_format_valid_ ? 1 : 0,
            self->opus_sink_.frame_cb ? 1 : 0);
    }
    if (!self->audio_format_valid_ || !self->opus_sink_.frame_cb) return;
    self->media_.recordIncomingAudioPacket();
    self->opus_sink_.frame_cb(buf, buf_size, self->opus_sink_.user);
}

void PsMediaBridge::decodedAudioSettingsCb(uint32_t channels, uint32_t rate, void* user) {
    auto* self = static_cast<PsMediaBridge*>(user);
    if (!self || channels != 2 || rate != 48000) return;
    self->audio_channels_ = static_cast<int>(channels);
    self->audio_sample_rate_ = static_cast<int>(rate);
}

void PsMediaBridge::decodedAudioFrameCb(int16_t* buf, size_t samples_count, void* user) {
    auto* self = static_cast<PsMediaBridge*>(user);
    if (self) self->onDecodedAudioFrame(buf, samples_count);
}

void PsMediaBridge::eventCb(ChiakiEvent* event, void* user) {
    auto* self = static_cast<PsMediaBridge*>(user);
    if (!self || !event) return;

    // Forward rumble events
    if (event->type == CHIAKI_EVENT_RUMBLE && self->rumble_cb_) {
        self->rumble_cb_(event->rumble.left, event->rumble.right);
    }

    // Forward all events to session handler
    if (self->event_cb_) {
        self->event_cb_(event);
    }
}

} // namespace lunar::ps

#endif
