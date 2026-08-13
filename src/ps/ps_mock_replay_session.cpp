#ifdef __SWITCH__

#include "ps_mock_replay_session.h"

#include "chiaki_log_adapter.h"
#include "../common.h"
#include "../diagnostics.h"

#include <cJSON.h>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

#ifndef LUNARNX_PS_MOCK_REPLAY
#define LUNARNX_PS_MOCK_REPLAY 0
#endif

namespace lunar::ps {
namespace {

constexpr const char* kFixturePath =
    "sdmc:/switch/LunarNX/ps_media_replay.mp4";

struct Demuxer {
    AVFormatContext* format = nullptr;
    AVIOContext* avio = nullptr;
    uint8_t* file_data = nullptr;
    size_t file_size = 0;
    size_t file_pos = 0;
    int video_index = -1;
    int audio_index = -1;

    static int readPacket(void* opaque, uint8_t* data, int size) {
        auto* self = static_cast<Demuxer*>(opaque);
        const size_t remaining = self->file_size - self->file_pos;
        if (remaining == 0) return AVERROR_EOF;
        const size_t count = std::min(remaining, static_cast<size_t>(size));
        std::memcpy(data, self->file_data + self->file_pos, count);
        self->file_pos += count;
        return static_cast<int>(count);
    }

    static int64_t seekPacket(void* opaque, int64_t offset, int whence) {
        auto* self = static_cast<Demuxer*>(opaque);
        if (whence == AVSEEK_SIZE) return static_cast<int64_t>(self->file_size);
        int64_t next = 0;
        if (whence == SEEK_SET) next = offset;
        else if (whence == SEEK_CUR) next = static_cast<int64_t>(self->file_pos) + offset;
        else if (whence == SEEK_END) next = static_cast<int64_t>(self->file_size) + offset;
        else return AVERROR(EINVAL);
        next = std::clamp<int64_t>(next, 0, static_cast<int64_t>(self->file_size));
        self->file_pos = static_cast<size_t>(next);
        return next;
    }

    bool open(stream::VideoCodec expected_codec) {
        FILE* input = std::fopen(kFixturePath, "rb");
        if (!input) return false;
        std::fseek(input, 0, SEEK_END);
        const long length = std::ftell(input);
        std::fseek(input, 0, SEEK_SET);
        if (length <= 0) {
            std::fclose(input);
            return false;
        }
        file_size = static_cast<size_t>(length);
        file_data = static_cast<uint8_t*>(av_malloc(file_size));
        if (!file_data || std::fread(file_data, 1, file_size, input) != file_size) {
            std::fclose(input);
            return false;
        }
        std::fclose(input);

        uint8_t* io_buffer = static_cast<uint8_t*>(av_malloc(32768));
        if (!io_buffer) return false;
        avio = avio_alloc_context(io_buffer, 32768, 0, this, readPacket, nullptr,
                                  seekPacket);
        if (!avio) {
            av_free(io_buffer);
            return false;
        }
        format = avformat_alloc_context();
        if (!format) return false;
        format->pb = avio;
        if (avformat_open_input(&format, nullptr, nullptr, nullptr) < 0 ||
            avformat_find_stream_info(format, nullptr) < 0) return false;
        video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1,
                                          nullptr, 0);
        audio_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1,
                                          nullptr, 0);
        if (video_index < 0 || audio_index < 0) return false;
        const auto* video = format->streams[video_index]->codecpar;
        const auto* audio = format->streams[audio_index]->codecpar;
        const AVCodecID expected_id = expected_codec == stream::VideoCodec::HEVC
            ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
        return video->codec_id == expected_id &&
               audio->codec_id == AV_CODEC_ID_OPUS &&
               audio->sample_rate == 48000 && audio->ch_layout.nb_channels == 2;
    }

    ~Demuxer() {
        if (format) {
            format->pb = nullptr;
            avformat_close_input(&format);
        }
        if (avio) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
        av_freep(&file_data);
    }
};

#if LUNARNX_PS_MOCK_REPLAY
bool configSelectsMockReplay() {
    FILE* input = std::fopen(lunar::get_config_path(), "rb");
    if (!input) return false;
    std::fseek(input, 0, SEEK_END);
    const long length = std::ftell(input);
    std::fseek(input, 0, SEEK_SET);
    if (length <= 0 || length > 64 * 1024) {
        std::fclose(input);
        return false;
    }
    std::string content(static_cast<size_t>(length), '\0');
    const bool read = std::fread(content.data(), 1, content.size(), input) == content.size();
    std::fclose(input);
    if (!read) return false;
    cJSON* root = cJSON_Parse(content.c_str());
    cJSON* value = root
        ? cJSON_GetObjectItemCaseSensitive(root, "ps_network_profile") : nullptr;
    const bool selected = cJSON_IsString(value) && value->valuestring &&
        std::strcmp(value->valuestring, "mock_replay") == 0;
    cJSON_Delete(root);
    return selected;
}
#endif

} // namespace

bool psMockReplayEnabled() {
#if LUNARNX_PS_MOCK_REPLAY
    return configSelectsMockReplay();
#else
    return false;
#endif
}

PsMockReplaySession::PsMockReplaySession(PsMediaBridge& bridge, int fps,
                                         stream::VideoCodec video_codec)
    : bridge_(bridge), fps_(std::clamp(fps, 1, 120)),
      video_codec_(video_codec) {}

PsMockReplaySession::~PsMockReplaySession() {
    stop();
}

bool PsMockReplaySession::start(PsSessionCallbacks callbacks) {
    if (running_.exchange(true)) return false;
    callbacks_ = std::move(callbacks);
    last_error_.clear();
    worker_ = std::thread([this]() { replayLoop(); });
    return true;
}

void PsMockReplaySession::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void PsMockReplaySession::replayLoop() {
    if (callbacks_.on_status) callbacks_.on_status("Opening mock replay fixture...");
    Demuxer demux;
    if (!demux.open(video_codec_)) {
        last_error_ = "Missing or invalid ps_media_replay.mp4";
        running_ = false;
        if (callbacks_.on_error) callbacks_.on_error(last_error_);
        return;
    }

    const char* filter_name = video_codec_ == stream::VideoCodec::HEVC
        ? "hevc_mp4toannexb" : "h264_mp4toannexb";
    const AVBitStreamFilter* filter = av_bsf_get_by_name(filter_name);
    AVBSFContext* bsf = nullptr;
    const auto* video_par = demux.format->streams[demux.video_index]->codecpar;
    if (!filter || av_bsf_alloc(filter, &bsf) < 0 ||
        avcodec_parameters_copy(bsf->par_in, video_par) < 0) {
        av_bsf_free(&bsf);
        last_error_ = std::string(filter_name) + " unavailable";
        running_ = false;
        if (callbacks_.on_error) callbacks_.on_error(last_error_);
        return;
    }
    bsf->time_base_in = demux.format->streams[demux.video_index]->time_base;
    if (av_bsf_init(bsf) < 0) {
        av_bsf_free(&bsf);
        last_error_ = std::string(filter_name) + " initialization failed";
        running_ = false;
        if (callbacks_.on_error) callbacks_.on_error(last_error_);
        return;
    }

    log_ = makeChiakiDiagnosticLog("ps-mock-replay");
    bridge_.initializeAudio(&log_);
    ChiakiAudioHeader header{};
    chiaki_audio_header_set(&header, 2, 16, 48000, 480);
    auto audio_sink = bridge_.audioSink();
    audio_sink.header_cb(&header, audio_sink.user);
    if (callbacks_.on_streaming) callbacks_.on_streaming();

    const auto wall_start = std::chrono::steady_clock::now();
    while (running_) {
        if (callbacks_.external_cancel && callbacks_.external_cancel()) break;
        AVPacket* packet = av_packet_alloc();
        if (!packet) break;
        const int read = av_read_frame(demux.format, packet);
        if (read < 0) {
            av_packet_free(&packet);
            break;
        }
        AVStream* stream = demux.format->streams[packet->stream_index];
        const int64_t timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        const int64_t due_us = timestamp == AV_NOPTS_VALUE ? 0 :
            av_rescale_q(timestamp, stream->time_base, AVRational{1, 1000000});
        const auto due = wall_start + std::chrono::microseconds(std::max<int64_t>(0, due_us));
        while (running_ && std::chrono::steady_clock::now() < due) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!running_) {
            av_packet_free(&packet);
            break;
        }

        if (packet->stream_index == demux.video_index &&
            av_bsf_send_packet(bsf, packet) >= 0) {
            AVPacket* annexb = av_packet_alloc();
            while (annexb && av_bsf_receive_packet(bsf, annexb) == 0) {
                bridge_.onVideoSample(annexb->data, annexb->size, 0, false);
                av_packet_unref(annexb);
            }
            av_packet_free(&annexb);
        } else if (packet->stream_index == demux.audio_index) {
            audio_sink.frame_cb(packet->data, packet->size, audio_sink.user);
        }
        av_packet_free(&packet);
    }
    av_bsf_free(&bsf);
    const bool stopped = !running_ ||
        (callbacks_.external_cancel && callbacks_.external_cancel());
    running_ = false;
    if (!stopped && callbacks_.on_disconnected) {
        callbacks_.on_disconnected("Mock replay complete");
    }
    diagnosticLog("ps-mock-replay", "session ended stopped=%d fps=%d", stopped, fps_);
}

} // namespace lunar::ps

#endif
