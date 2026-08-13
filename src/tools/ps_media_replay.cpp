#ifdef __SWITCH__

#include "../ps/chiaki_log_adapter.h"
#include "../ps/ps_media_bridge.h"
#include "../stream/media_pipeline.h"
#include "../stream/perf_stats.h"
#include "../stream/software_video_frame.h"
#include "../stream/stream_backend_provider.h"

#include <borealis.hpp>
#include <switch.h>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

#ifndef LUNARNX_PSMEDIA_BACKEND
#define LUNARNX_PSMEDIA_BACKEND 2
#endif

constexpr auto kVideoBackend = static_cast<lunar::stream::VideoBackend>(
    LUNARNX_PSMEDIA_BACKEND);

constexpr const char* kFixturePath =
    "sdmc:/switch/LunarNX/ps_media_replay.mp4";
constexpr const char* kResultPath =
    "sdmc:/switch/LunarNX/ps_media_replay.log";

void writeResult(const char* status, const char* format, ...) {
    FILE* output = std::fopen(kResultPath, "a");
    if (!output) return;
    std::fprintf(output, "status=%s ", status);
    va_list args;
    va_start(args, format);
    std::vfprintf(output, format, args);
    va_end(args);
    std::fputc('\n', output);
    std::fclose(output);
}

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

    bool open() {
        FILE* input = std::fopen(kFixturePath, "rb");
        if (!input) return false;
        std::fseek(input, 0, SEEK_END);
        file_size = static_cast<size_t>(std::ftell(input));
        std::fseek(input, 0, SEEK_SET);
        file_data = static_cast<uint8_t*>(av_malloc(file_size));
        if (!file_data || std::fread(file_data, 1, file_size, input) != file_size) {
            std::fclose(input);
            return false;
        }
        std::fclose(input);
        uint8_t* io_buffer = static_cast<uint8_t*>(av_malloc(32768));
        avio = avio_alloc_context(io_buffer, 32768, 0, this, readPacket, nullptr,
                                  seekPacket);
        format = avformat_alloc_context();
        if (!avio || !format) return false;
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
        return (video->codec_id == AV_CODEC_ID_H264 ||
                video->codec_id == AV_CODEC_ID_HEVC) &&
               audio->codec_id == AV_CODEC_ID_OPUS &&
               audio->sample_rate == 48000 && audio->ch_layout.nb_channels == 2;
    }

    void close() {
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

    ~Demuxer() { close(); }
};

class ReplayActivity final : public brls::Activity {
public:
    ~ReplayActivity() override {
        alive_->store(false);
        stop();
    }

    brls::View* createContentView() override {
        class ReplayView final : public brls::Box {
        public:
            explicit ReplayView(ReplayActivity* owner)
                : brls::Box(brls::Axis::COLUMN), owner_(owner) {}

            ~ReplayView() override {
                if (nvg_image_ >= 0) {
                    nvgDeleteImage(brls::Application::getNVGContext(), nvg_image_);
                }
            }

            void draw(NVGcontext* vg, float x, float y, float width, float height,
                      brls::Style style, brls::FrameContext* context) override {
                brls::Box::draw(vg, x, y, width, height, style, context);
                if (lunar::stream::usesZeroCopyRender(kVideoBackend)) {
                    if (owner_) owner_->presentVideoFrame();
                    return;
                }

                auto frame = lunar::stream::SoftwareVideoFrameSink::instance().snapshot();
                if (frame.empty()) return;
                if (nvg_image_ < 0 || frame.width != image_width_ ||
                    frame.height != image_height_) {
                    if (nvg_image_ >= 0) nvgDeleteImage(vg, nvg_image_);
                    nvg_image_ = nvgCreateImageRGBA(vg, frame.width, frame.height,
                                                    0, frame.rgba.data());
                    image_width_ = frame.width;
                    image_height_ = frame.height;
                    last_generation_ = frame.generation;
                } else if (last_generation_ != frame.generation) {
                    nvgUpdateImage(vg, nvg_image_, frame.rgba.data());
                    last_generation_ = frame.generation;
                }
                if (nvg_image_ < 0) return;

                const float scale = std::min(width / static_cast<float>(frame.width),
                                             height / static_cast<float>(frame.height));
                const float draw_width = static_cast<float>(frame.width) * scale;
                const float draw_height = static_cast<float>(frame.height) * scale;
                const float draw_x = x + (width - draw_width) * 0.5f;
                const float draw_y = y + (height - draw_height) * 0.5f;
                const NVGpaint image = nvgImagePattern(vg, draw_x, draw_y,
                    draw_width, draw_height, 0.0f, nvg_image_, 1.0f);
                nvgBeginPath(vg);
                nvgRect(vg, draw_x, draw_y, draw_width, draw_height);
                nvgFillPaint(vg, image);
                nvgFill(vg);
            }

        private:
            ReplayActivity* owner_;
            int nvg_image_ = -1;
            int image_width_ = 0;
            int image_height_ = 0;
            uint64_t last_generation_ = 0;
        };

        auto* root = new ReplayView(this);
        root->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
        root->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
        root->setFocusable(true);
        root->setHideHighlight(true);
        if (!lunar::stream::usesZeroCopyRender(kVideoBackend)) {
            root->setBackgroundColor(nvgRGBA(0, 0, 0, 255));
        }
        root->registerAction("PLC", brls::ControllerButton::BUTTON_A,
            [this](brls::View*) { inject_plc_ = true; return true; });
        root->registerAction("Video loss", brls::ControllerButton::BUTTON_X,
            [this](brls::View*) { inject_video_loss_ = true; return true; });
        root->registerAction("Restart", brls::ControllerButton::BUTTON_Y,
            [this](brls::View*) { restart_requested_ = true; return true; });
        root->registerAction("Exit", brls::ControllerButton::BUTTON_B,
            [](brls::View*) { brls::Application::quit(); return true; });
        status_ = new brls::Label();
        status_->setText("Opening PS media replay fixture...");
        status_->setFontSize(20);
        status_->setHeight(80);
        root->addView(status_);
        start();
        return root;
    }

    void onContentAvailable() override {
        brls::sync([this]() { refreshStatus(); });
    }

private:
    std::unique_ptr<lunar::stream::StreamBackendProvider> provider_;
    std::unique_ptr<lunar::stream::MediaPipeline> media_;
    std::unique_ptr<lunar::ps::PsMediaBridge> bridge_;
    lunar::stream::PerfStats perf_;
    ChiakiLog chiaki_log_{};
    brls::Label* status_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> first_video_{false};
    std::atomic<bool> inject_plc_{false};
    std::atomic<bool> inject_video_loss_{false};
    std::atomic<bool> restart_requested_{false};
    std::atomic<uint64_t> video_input_{0};
    std::atomic<uint64_t> audio_input_{0};
    std::atomic<uint64_t> plc_input_{0};
    std::atomic<uint64_t> video_loss_input_{0};
    std::atomic<uint64_t> restart_count_{0};
    std::mutex pipeline_mutex_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);

    void presentVideoFrame() {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        if (media_) media_->presentVideoFrame();
    }

    bool initializePipeline(int width, int height,
                            lunar::stream::VideoCodec video_codec) {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        provider_ = lunar::stream::StreamBackendProvider::createDefault();
        media_ = std::make_unique<lunar::stream::MediaPipeline>(*provider_);
        lunar::stream::MediaPipelineOptions options;
        options.video_codec = video_codec;
        options.video_backend = kVideoBackend;
        perf_.reset();
        first_video_ = false;
        if (!media_->initialize(width, height, &perf_, options)) return false;
        media_->setVideoReadyCallback([this]() { first_video_ = true; });
        bridge_ = std::make_unique<lunar::ps::PsMediaBridge>(*media_, 60);
        chiaki_log_ = lunar::ps::makeChiakiDiagnosticLog("ps-media-replay");
        bridge_->initializeAudio(&chiaki_log_);
        ChiakiAudioHeader header{};
        chiaki_audio_header_set(&header, 2, 16, 48000, 480);
        auto sink = bridge_->audioSink();
        sink.header_cb(&header, sink.user);
        return true;
    }

    void destroyPipeline() {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        bridge_.reset();
        if (media_) media_->shutdown();
        media_.reset();
        provider_.reset();
    }

    void start() {
        running_ = true;
        worker_ = std::thread([this]() { replayLoop(); });
    }

    void stop() {
        running_ = false;
        if (worker_.joinable()) worker_.join();
        destroyPipeline();
    }

    void replayLoop() {
        Demuxer demux;
        if (!demux.open()) {
            writeResult("FAIL", "reason=fixture_open path=%s", kFixturePath);
            setStatus("FAIL: missing or invalid ps_media_replay.mp4");
            return;
        }
        const auto* video_par = demux.format->streams[demux.video_index]->codecpar;
        const auto video_codec = video_par->codec_id == AV_CODEC_ID_HEVC
            ? lunar::stream::VideoCodec::HEVC : lunar::stream::VideoCodec::H264;
        if (!initializePipeline(video_par->width, video_par->height, video_codec)) {
            writeResult("FAIL", "reason=pipeline_init");
            setStatus("FAIL: media pipeline initialization failed");
            return;
        }

        const char* filter_name = video_codec == lunar::stream::VideoCodec::HEVC
            ? "hevc_mp4toannexb" : "h264_mp4toannexb";
        const AVBitStreamFilter* filter = av_bsf_get_by_name(filter_name);
        AVBSFContext* bsf = nullptr;
        if (!filter || av_bsf_alloc(filter, &bsf) < 0 ||
            avcodec_parameters_copy(bsf->par_in, video_par) < 0) {
            av_bsf_free(&bsf);
            writeResult("FAIL", "reason=video_bsf_init codec=%s",
                        lunar::stream::videoCodecName(video_codec));
            setStatus(std::string("FAIL: ") + filter_name + " unavailable");
            return;
        }
        bsf->time_base_in = demux.format->streams[demux.video_index]->time_base;
        if (av_bsf_init(bsf) < 0) {
            av_bsf_free(&bsf);
            writeResult("FAIL", "reason=video_bsf_init codec=%s",
                        lunar::stream::videoCodecName(video_codec));
            setStatus(std::string("FAIL: ") + filter_name + " unavailable");
            return;
        }
        const auto wall_start = std::chrono::steady_clock::now();
        bool reported = false;
        bool automatic_plc_done = false;
        bool automatic_loss_done = false;
        bool automatic_restart_done = false;
        while (running_) {
            AVPacket* packet = av_packet_alloc();
            if (!packet) break;
            const int read = av_read_frame(demux.format, packet);
            if (read < 0) {
                av_packet_free(&packet);
                break;
            }
            AVStream* stream = demux.format->streams[packet->stream_index];
            const int64_t packet_ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            const int64_t due_us = packet_ts == AV_NOPTS_VALUE ? 0 :
                av_rescale_q(packet_ts, stream->time_base, AVRational{1, 1000000});
            const auto due = wall_start + std::chrono::microseconds(std::max<int64_t>(0, due_us));
            if (std::chrono::steady_clock::now() < due) std::this_thread::sleep_until(due);

            const auto elapsed = std::chrono::steady_clock::now() - wall_start;
            if (!automatic_plc_done && elapsed >= std::chrono::seconds(2)) {
                inject_plc_ = true;
                automatic_plc_done = true;
            }
            if (!automatic_loss_done && elapsed >= std::chrono::seconds(3)) {
                inject_video_loss_ = true;
                automatic_loss_done = true;
            }
            if (!automatic_restart_done && elapsed >= std::chrono::seconds(5)) {
                restart_requested_ = true;
                automatic_restart_done = true;
            }

            if (restart_requested_.exchange(false)) {
                destroyPipeline();
                if (!initializePipeline(video_par->width, video_par->height,
                                        video_codec)) {
                    av_packet_free(&packet);
                    writeResult("FAIL", "reason=pipeline_restart backend=%s",
                                lunar::stream::videoBackendName(kVideoBackend));
                    setStatus("FAIL: media pipeline restart failed");
                    break;
                }
                restart_count_++;
            }

            std::lock_guard<std::mutex> lock(pipeline_mutex_);
            if (!bridge_) {
                av_packet_free(&packet);
                continue;
            }
            if (packet->stream_index == demux.video_index) {
                if (av_bsf_send_packet(bsf, packet) >= 0) {
                    AVPacket* annexb = av_packet_alloc();
                    while (annexb && av_bsf_receive_packet(bsf, annexb) == 0) {
                        const int32_t lost = inject_video_loss_.exchange(false) ? 1 : 0;
                        if (lost > 0) video_loss_input_++;
                        const auto input_index = video_input_.load();
                        if (input_index < 8) {
                            writeResult("TRACE",
                                "backend=%s video_index=%llu bytes=%d lost=%d",
                                lunar::stream::videoBackendName(kVideoBackend),
                                static_cast<unsigned long long>(input_index),
                                annexb->size, lost);
                        }
                        if (bridge_->onVideoSample(annexb->data, annexb->size, lost, false))
                            video_input_++;
                        av_packet_unref(annexb);
                    }
                    av_packet_free(&annexb);
                }
            } else if (packet->stream_index == demux.audio_index) {
                auto sink = bridge_->audioSink();
                if (inject_plc_.exchange(false)) {
                    sink.frame_cb(nullptr, 0, sink.user);
                    plc_input_++;
                }
                sink.frame_cb(packet->data, packet->size, sink.user);
                audio_input_++;
            }
            av_packet_free(&packet);

            if (!reported && elapsed >= std::chrono::seconds(12)) {
                reported = true;
                const bool passed = first_video_.load() && perf_.video_frames.load() > 0 &&
                    perf_.audio_frames.load() > 0 && perf_.video_decode_errors.load() == 0 &&
                    plc_input_.load() > 0 && video_loss_input_.load() > 0 &&
                    restart_count_.load() > 0;
                writeResult(passed ? "PASS" : "FAIL",
                    "backend=%s video_in=%llu audio_in=%llu plc=%llu loss=%llu restarts=%llu rendered=%u pcm=%u decode_errors=%u video_drops=%u audio_drops=%u",
                    lunar::stream::videoBackendName(kVideoBackend),
                    static_cast<unsigned long long>(video_input_.load()),
                    static_cast<unsigned long long>(audio_input_.load()),
                    static_cast<unsigned long long>(plc_input_.load()),
                    static_cast<unsigned long long>(video_loss_input_.load()),
                    static_cast<unsigned long long>(restart_count_.load()),
                    perf_.video_frames.load(), perf_.audio_frames.load(),
                    perf_.video_decode_errors.load(), perf_.video_frame_drops.load(),
                    perf_.audio_drops.load());
            }
            if ((video_input_.load() + audio_input_.load()) % 120 == 0) refreshStatusAsync();
        }
        av_bsf_free(&bsf);
        setStatus("Replay complete. Press + to exit.");
    }

    void refreshStatus() {
        if (!status_) return;
        status_->setText(
            "PS callback replay | video in " + std::to_string(video_input_.load()) +
            " rendered " + std::to_string(perf_.video_frames.load()) +
            " | Opus in " + std::to_string(audio_input_.load()) +
            " PCM " + std::to_string(perf_.audio_frames.load()) +
            " | " + lunar::stream::videoBackendOverlayName(kVideoBackend) +
            " | auto PLC/loss/restart | A/X/Y manual  B exit");
    }

    void refreshStatusAsync() {
        auto alive = alive_;
        brls::sync([this, alive]() {
            if (alive->load()) refreshStatus();
        });
    }

    void setStatus(std::string text) {
        auto alive = alive_;
        brls::sync([this, alive, text = std::move(text)]() {
            if (alive->load() && status_) status_->setText(text);
        });
    }
};

} // namespace

int main() {
    std::remove(kResultPath);
    if (!brls::Application::init()) return EXIT_FAILURE;
    brls::Application::createWindow("LunarNX PS Media Replay");
    brls::Application::setGlobalQuit(false);
    brls::Application::pushActivity(new ReplayActivity());
    while (brls::Application::mainLoop()) {}
    return EXIT_SUCCESS;
}

#endif
