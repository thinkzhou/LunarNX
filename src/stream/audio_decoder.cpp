#include "audio_decoder.h"
#include "../diagnostics.h"
#include "perf_stats.h"

#include <atomic>
#include <cstdio>
#include <utility>

#include <opus/opus_multistream.h>

namespace lunar::stream {

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kStreams = 1;
constexpr int kCoupledStreams = 1;
constexpr int kSamplesPerFrame = 960;
constexpr int kMaxSamplesPerFrame = 5760;
constexpr int kAudioDecodeLogLimit = 12;

std::atomic<int> g_audio_decode_logs{0};

bool shouldLogAudioDecode(int index) {
    return index < kAudioDecodeLogLimit;
}

} // namespace

AudioDecoder::AudioDecoder() = default;
AudioDecoder::~AudioDecoder() { shutdown(); }

bool AudioDecoder::initialize() {
    shutdown();
    g_audio_decode_logs = 0;

    unsigned char mapping[kChannels] = {0, 1};
    int error = OPUS_OK;
    OpusMSDecoder* decoder = opus_multistream_decoder_create(
        kSampleRate,
        kChannels,
        kStreams,
        kCoupledStreams,
        mapping,
        &error);
    if (!decoder || error != OPUS_OK) {
        fprintf(stderr, "[audio] Opus decoder init failed: %s\n",
                opus_strerror(error));
        lunar::diagnosticLog("audio-decoder",
                             "opus init failed error=%d %s",
                             error,
                             opus_strerror(error));
        if (decoder) opus_multistream_decoder_destroy(decoder);
        return false;
    }

    decoder_ = decoder;
    initialized_ = true;
    fprintf(stderr, "[audio] libopus multistream decoder initialized, 48kHz stereo\n");
    lunar::diagnosticLog("audio-decoder",
                         "opus init done rate=%d channels=%d streams=%d coupled=%d frame=%d",
                         kSampleRate,
                         kChannels,
                         kStreams,
                         kCoupledStreams,
                         kSamplesPerFrame);
    return true;
}

bool AudioDecoder::decode(const uint8_t* data, size_t len, uint64_t timestamp) {
    if (!initialized_ || !decoder_ || !data || len == 0) return false;

    const int packet_samples = opus_packet_get_nb_samples(
        data,
        static_cast<opus_int32>(len),
        kSampleRate);
    if (packet_samples <= 0) {
        lunar::diagnosticLog("audio-decoder",
                             "invalid opus packet duration ret=%d len=%zu",
                             packet_samples,
                             len);
        if (perf_) perf_->recordAudioDrop();
        return false;
    }
    return decodeInternal(data, len, packet_samples, timestamp, false);
}

bool AudioDecoder::decodeMissing(uint64_t timestamp) {
    if (!initialized_ || !decoder_) return false;
    return decodeInternal(nullptr,
                          0,
                          static_cast<int>(last_frame_samples_),
                          timestamp,
                          true);
}

void AudioDecoder::reset() {
    if (!initialized_ || !decoder_) return;
    const int result = opus_multistream_decoder_ctl(
        static_cast<OpusMSDecoder*>(decoder_), OPUS_RESET_STATE);
    if (result != OPUS_OK) {
        lunar::diagnosticLog("audio-decoder", "opus reset failed ret=%d", result);
    }
    last_frame_samples_ = kSamplesPerFrame;
}

bool AudioDecoder::decodeInternal(const uint8_t* data,
                                  size_t len,
                                  int frame_size,
                                  uint64_t timestamp,
                                  bool plc) {
    if (frame_size <= 0 || frame_size > kMaxSamplesPerFrame) return false;

    const int log_index = g_audio_decode_logs.fetch_add(1);
    const bool log = shouldLogAudioDecode(log_index);
    if (log) {
        lunar::diagnosticLog("audio-decoder",
                             "opus %s begin index=%d len=%zu ts=%llu frame=%d",
                             plc ? "plc" : "decode",
                             log_index,
                             len,
                             static_cast<unsigned long long>(timestamp),
                             frame_size);
    }

    AudioFrame frame;
    frame.sample_rate = kSampleRate;
    frame.channels = kChannels;
    frame.timestamp = timestamp;
    frame.pcm_data.resize(
        static_cast<size_t>(frame_size) * kChannels * sizeof(opus_int16));

    auto* pcm = reinterpret_cast<opus_int16*>(frame.pcm_data.data());
    int decoded_samples = opus_multistream_decode(
        static_cast<OpusMSDecoder*>(decoder_),
        data,
        static_cast<opus_int32>(len),
        pcm,
        frame_size,
        0);

    if (log) {
        lunar::diagnosticLog("audio-decoder",
                             "opus decode ret=%d",
                             decoded_samples);
    }
    if (decoded_samples <= 0) {
        lunar::diagnosticLog("audio-decoder",
                             "opus %s failed index=%d ret=%d %s len=%zu",
                             plc ? "plc" : "decode",
                             log_index,
                             decoded_samples,
                             opus_strerror(decoded_samples),
                             len);
        if (perf_) perf_->recordAudioDrop();
        return false;
    }

    frame.sample_count = static_cast<size_t>(decoded_samples);
    last_frame_samples_ = frame.sample_count;
    frame.pcm_data.resize(
        frame.sample_count * static_cast<size_t>(kChannels) * sizeof(opus_int16));

    if (on_frame_) {
        if (perf_) perf_->recordAudioFrame();
        if (log) {
            lunar::diagnosticLog("audio-decoder",
                                 "frame callback begin pcm=%zu samples=%zu",
                                 frame.pcm_data.size(),
                                 frame.sample_count);
        }
        on_frame_(frame);
        if (log) lunar::diagnosticLog("audio-decoder", "frame callback done");
    }

    if (log) lunar::diagnosticLog("audio-decoder", "opus decode done index=%d", log_index);
    return true;
}

void AudioDecoder::setCallback(FrameCallback cb) { on_frame_ = std::move(cb); }

void AudioDecoder::shutdown() {
    if (decoder_) {
        opus_multistream_decoder_destroy(static_cast<OpusMSDecoder*>(decoder_));
        decoder_ = nullptr;
    }
    initialized_ = false;
    last_frame_samples_ = kSamplesPerFrame;
}

} // namespace lunar::stream
