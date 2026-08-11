#include "video_decoder.h"
#include "../diagnostics.h"
#include "perf_stats.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
}

namespace lunar::stream {

namespace {

#ifndef LUNARNX_VIDEO_DECODE_LOG_LIMIT
#define LUNARNX_VIDEO_DECODE_LOG_LIMIT 16
#endif

#ifndef LUNARNX_VIDEO_ERROR_LOG_LIMIT
#define LUNARNX_VIDEO_ERROR_LOG_LIMIT 16
#endif

constexpr int kVideoDecodeLogLimit = LUNARNX_VIDEO_DECODE_LOG_LIMIT;
constexpr int kVideoErrorLogLimit = LUNARNX_VIDEO_ERROR_LOG_LIMIT;
constexpr int kH264WaitLogLimit = 16;
std::atomic<int> g_video_decode_logs{0};
std::atomic<int> g_video_transfer_logs{0};

bool shouldLogVideoDecode(int index) {
    return index < kVideoDecodeLogLimit;
}

void logVideoDecodeError(const char* label, int err, int& count) {
    if (count++ >= kVideoErrorLogLimit) return;
    char error[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, error, sizeof(error));
    lunar::diagnosticLog("video", "%s err=%d %s", label, err, error);
}

int softwareDecoderThreadCount() {
#ifdef __SWITCH__
    return 1;
#else
    return 4;
#endif
}

void logSoftwareAllocationProbe(const AVCodec* codec) {
    lunar::diagnosticLog("video",
                         "software codec name=%s long=%s capabilities=0x%x max_lowres=%u",
                         codec && codec->name ? codec->name : "(null)",
                         codec && codec->long_name ? codec->long_name : "(null)",
                         codec ? codec->capabilities : 0,
                         codec ? static_cast<unsigned>(codec->max_lowres) : 0);

#ifdef __SWITCH__
    static constexpr size_t kProbeSizes[] = {
        1 * 1024 * 1024,
        16 * 1024 * 1024,
        64 * 1024 * 1024,
    };
    for (size_t size : kProbeSizes) {
        void* malloc_ptr = std::malloc(size);
        lunar::diagnosticLog("video",
                             "software alloc probe malloc size=%zu ok=%s",
                             size,
                             malloc_ptr ? "true" : "false");
        std::free(malloc_ptr);

        void* av_ptr = av_malloc(size);
        lunar::diagnosticLog("video",
                             "software alloc probe av_malloc size=%zu ok=%s",
                             size,
                             av_ptr ? "true" : "false");
        av_free(av_ptr);
    }
#endif
}

struct H264AccessUnitInfo {
    bool has_sps = false;
    bool has_pps = false;
    bool has_idr = false;
    bool has_vcl = false;
    int nal_count = 0;
    std::string nal_types;
};

void logDecodeDrop(PerfStats* perf,
                   const char* reason,
                   int error_code,
                   uint32_t error_flags,
                   uint64_t timestamp,
                   size_t access_unit_bytes,
                   const H264AccessUnitInfo* au = nullptr,
                   int width = 0,
                   int height = 0) {
    if (perf) {
        perf->logVideoDropDiagnostic(
            "decode_error",
            reason,
            error_code,
            error_flags,
            timestamp,
            access_unit_bytes,
            width,
            height,
            au && !au->nal_types.empty() ? au->nal_types.c_str() : nullptr,
            au && au->has_idr);
        return;
    }
    lunar::dropDiagnosticLog("video-drop",
                             "source=decode_error reason=%s err=%d flags=0x%x "
                             "pts_ns=%llu au_bytes=%zu frame=%dx%d",
                             reason ? reason : "unknown",
                             error_code,
                             error_flags,
                             static_cast<unsigned long long>(timestamp),
                             access_unit_bytes,
                             width,
                             height);
}

bool findStartCode(const uint8_t* data,
                   size_t len,
                   size_t from,
                   size_t& pos,
                   size_t& code_size) {
    for (size_t i = from; i + 3 <= len; ++i) {
        if (data[i] != 0x00 || data[i + 1] != 0x00) continue;
        if (data[i + 2] == 0x01) {
            pos = i;
            code_size = 3;
            return true;
        }
        if (i + 4 <= len && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            pos = i;
            code_size = 4;
            return true;
        }
    }
    return false;
}

void appendNalType(std::string& out, uint8_t type) {
    if (out.size() > 96) return;
    if (!out.empty()) out.push_back(',');
    char buf[8] = {};
    std::snprintf(buf, sizeof(buf), "%u", type);
    out += buf;
}

H264AccessUnitInfo inspectH264AccessUnit(const uint8_t* data, size_t len) {
    H264AccessUnitInfo info;
    if (!data || len == 0) return info;

    size_t pos = 0;
    size_t code_size = 0;
    if (!findStartCode(data, len, 0, pos, code_size)) {
        const uint8_t type = data[0] & 0x1f;
        info.nal_count = 1;
        info.has_sps = type == 7;
        info.has_pps = type == 8;
        info.has_idr = type == 5;
        info.has_vcl = type >= 1 && type <= 5;
        appendNalType(info.nal_types, type);
        return info;
    }

    while (pos < len) {
        const size_t nalu_start = pos + code_size;
        size_t next = len;
        size_t next_code_size = 0;
        findStartCode(data, len, nalu_start, next, next_code_size);
        if (nalu_start < next && nalu_start < len) {
            const uint8_t type = data[nalu_start] & 0x1f;
            info.nal_count++;
            info.has_sps = info.has_sps || type == 7;
            info.has_pps = info.has_pps || type == 8;
            info.has_idr = info.has_idr || type == 5;
            info.has_vcl = info.has_vcl || (type >= 1 && type <= 5);
            appendNalType(info.nal_types, type);
        }
        if (next >= len) break;
        pos = next;
        code_size = next_code_size;
    }
    return info;
}

} // namespace

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { shutdown(); }

bool VideoDecoder::initialize(int width, int height) {
    g_video_decode_logs = 0;
    g_video_transfer_logs = 0;
    h264_decoder_ready_ = false;
    h264_seen_sps_ = false;
    h264_seen_pps_ = false;
    h264_parameter_sets_.clear();
    h264_parameter_sets_pending_ = false;
    h264_wait_log_count_ = 0;
    h264_error_log_count_ = 0;
#ifdef __SWITCH__
    if (usesHardwareDecode(video_backend_)) {
    // =========================================================================
    // Tegra X1 NVDEC hardware decoding
    // Reference: Moonlight-Switch FFmpegVideoDecoder
    // =========================================================================

    // Create NVDEC hardware device context
    lunar::diagnosticLog("video", "NVDEC hwdevice_ctx_create begin");
    int err = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_NVTEGRA,
                                      nullptr, nullptr, 0);
    if (err < 0) {
        fprintf(stderr, "[video] NVDEC hwdevice_ctx_create failed: %d\n", err);
        lunar::diagnosticLog("video", "NVDEC hwdevice_ctx_create failed err=%d", err);
        return false;
    }
    lunar::diagnosticLog("video", "NVDEC hwdevice_ctx_create done");

    // Find H.264 decoder
    lunar::diagnosticLog("video", "avcodec_find_decoder H264 begin");
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "[video] H.264 decoder not found\n");
        lunar::diagnosticLog("video", "H264 decoder not found");
        return false;
    }
    lunar::diagnosticLog("video", "avcodec_find_decoder H264 done");

    lunar::diagnosticLog("video", "avcodec_alloc_context3 begin");
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        fprintf(stderr, "[video] avcodec_alloc_context3 failed\n");
        lunar::diagnosticLog("video", "avcodec_alloc_context3 failed");
        return false;
    }
    lunar::diagnosticLog("video", "avcodec_alloc_context3 done");

    ctx->width = width;
    ctx->height = height;

    // Enable hardware decoding
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

    // NVDEC output format — hardware frames, zero-copy to GPU
    ctx->pix_fmt = AV_PIX_FMT_NVTEGRA;

    // Hardware decoding is single-threaded (NVDEC is a dedicated engine)
    ctx->thread_count = 1;
    ctx->thread_type = FF_THREAD_FRAME;
    ctx->extra_hw_frames = 16;

    // Accept possibly-corrupted frames (important for streaming)
    ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL | AV_CODEC_FLAG2_FAST;

    lunar::diagnosticLog("video", "avcodec_open2 NVDEC begin");
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        fprintf(stderr, "[video] avcodec_open2 (NVDEC) failed\n");
        lunar::diagnosticLog("video", "avcodec_open2 NVDEC failed");
        avcodec_free_context(&ctx);
        return false;
    }
    lunar::diagnosticLog("video", "avcodec_open2 NVDEC done");

    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    if (!parser) {
        fprintf(stderr, "[video] av_parser_init failed\n");
        lunar::diagnosticLog("video", "av_parser_init failed");
        avcodec_free_context(&ctx);
        return false;
    }
    parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    lunar::diagnosticLog("video", "av_parser_init H264 done");

    codec_ctx_ = ctx;
    parser_ = parser;
    initialized_ = true;

    fprintf(stderr, "[video] NVDEC H.264 decoder initialized, %dx%d\n", width, height);
    lunar::diagnosticLog("video", "NVDEC H264 decoder initialized width=%d height=%d extra_hw=%d",
                         width,
                         height,
                         ctx->extra_hw_frames);
    return true;
    }
#else
    video_backend_ = VideoBackend::Software;
#endif
    // =========================================================================
    // Software H.264 decoding. Used by desktop and by Switch when selected for
    // simulator/debug playback.
    // =========================================================================
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "[video] H.264 decoder not found\n");
        lunar::diagnosticLog("video", "software H264 decoder not found");
        return false;
    }
    logSoftwareAllocationProbe(codec);

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        fprintf(stderr, "[video] avcodec_alloc_context3 failed\n");
        lunar::diagnosticLog("video", "software avcodec_alloc_context3 failed");
        return false;
    }

    ctx->width = width;
    ctx->height = height;
    ctx->thread_count = softwareDecoderThreadCount();

    int open_ret = avcodec_open2(ctx, codec, nullptr);
    if (open_ret < 0) {
        fprintf(stderr, "[video] avcodec_open2 failed\n");
        logVideoDecodeError("software avcodec_open2 failed", open_ret,
                            h264_error_log_count_);
        avcodec_free_context(&ctx);
        return false;
    }

    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    if (!parser) {
        fprintf(stderr, "[video] av_parser_init failed\n");
        lunar::diagnosticLog("video", "software av_parser_init failed");
        avcodec_free_context(&ctx);
        return false;
    }
    parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

    codec_ctx_ = ctx;
    parser_ = parser;
    initialized_ = true;

    fprintf(stderr, "[video] Software H.264 decoder initialized, %dx%d (%d threads)\n",
            width, height, ctx->thread_count);
    lunar::diagnosticLog("video",
                         "Software H264 decoder initialized width=%d height=%d threads=%d",
                         width,
                         height,
                         ctx->thread_count);
    return true;
}

bool VideoDecoder::deliverFrame(AVFrame* frame,
                                uint64_t timestamp,
                                int log_index,
                                bool log) {
    if (!frame) return false;

    if (frame->decode_error_flags != 0) {
        if (perf_) perf_->recordVideoDecodeError();
        logDecodeDrop(perf_,
                      "decoded_frame_corrupt_flags",
                      0,
                      frame->decode_error_flags,
                      timestamp,
                      0,
                      nullptr,
                      frame->width,
                      frame->height);
        lunar::diagnosticLog("video",
                             "drop corrupt decoded frame index=%d flags=0x%x",
                             log_index,
                             frame->decode_error_flags);
        av_frame_free(&frame);
        return false;
    }

    AVFrame* callback_frame = frame;
    const uint64_t probe_frame_index = static_cast<uint64_t>(log_index) + 1;

#ifdef __SWITCH__
    if (video_backend_ == VideoBackend::HardwareCopyOut &&
        frame->format == AV_PIX_FMT_NVTEGRA) {
        AVFrame* sw_frame = av_frame_alloc();
        if (!sw_frame) {
            if (perf_) perf_->recordVideoDecodeError();
            logDecodeDrop(perf_, "nvdec_transfer_frame_alloc", 0, 0,
                          timestamp, 0, nullptr, frame->width, frame->height);
            lunar::diagnosticLog("video", "NVDEC transfer alloc failed index=%d", log_index);
            av_frame_free(&frame);
            return false;
        }

        const auto transfer_start = std::chrono::high_resolution_clock::now();
        int transfer_ret = av_hwframe_transfer_data(sw_frame, frame, 0);
        const auto transfer_end = std::chrono::high_resolution_clock::now();
        if (transfer_ret < 0) {
            if (perf_) perf_->recordVideoDecodeError();
            logDecodeDrop(perf_, "nvdec_transfer_failed", transfer_ret, 0,
                          timestamp, 0, nullptr, frame->width, frame->height);
            logVideoDecodeError("NVDEC av_hwframe_transfer_data failed",
                                transfer_ret,
                                h264_error_log_count_);
            av_frame_free(&sw_frame);
            av_frame_free(&frame);
            return false;
        }
        av_frame_copy_props(sw_frame, frame);
        if (perf_) {
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                transfer_end - transfer_start).count();
            perf_->recordDecodeLatencyNs(static_cast<uint64_t>(ns));
        }
        const bool transfer_log = g_video_transfer_logs.fetch_add(1) < 8;
        if (log || transfer_log) {
            lunar::diagnosticLog("video",
                                 "NVDEC transfer frame index=%d hw_format=%d sw_format=%d width=%d height=%d lines=%d/%d",
                                 log_index,
                                 frame->format,
                                 sw_frame->format,
                                 sw_frame->width,
                                 sw_frame->height,
                                 sw_frame->linesize[0],
                                 sw_frame->linesize[1]);
        }
        av_frame_free(&frame);
        callback_frame = sw_frame;
    }
#endif

    if (log) {
        lunar::diagnosticLog("video",
                             "deliver frame index=%d width=%d height=%d format=%d",
                             log_index,
                             callback_frame->width,
                             callback_frame->height,
                             callback_frame->format);
    }

    if (on_frame_) {
        if (lunar::shouldSampleCloud1080CrashProbe(probe_frame_index)) {
            lunar::cloud1080CrashProbeLog(
                "crash-probe",
                "DEBUG-c1080 phase=decoded-frame-callback-begin au=%llu "
                "frame=%dx%d format=%d pts_ns=%llu",
                static_cast<unsigned long long>(probe_frame_index),
                callback_frame->width,
                callback_frame->height,
                callback_frame->format,
                static_cast<unsigned long long>(timestamp));
        }
        VideoFrame vf;
        vf.width = callback_frame->width;
        vf.height = callback_frame->height;
        vf.format = callback_frame->format;
        vf.timestamp = timestamp;
        bool hardware_frame = false;
#ifdef __SWITCH__
        hardware_frame = callback_frame->format == AV_PIX_FMT_NVTEGRA;
#endif
        if (!hardware_frame) {
            for (int i = 0; i < 4; i++) {
                vf.data[i] = callback_frame->data[i];
                vf.linesize[i] = callback_frame->linesize[i];
            }
        }
        vf.avframe = callback_frame;
        on_frame_(vf);
        if (lunar::shouldSampleCloud1080CrashProbe(probe_frame_index)) {
            lunar::cloud1080CrashProbeLog(
                "crash-probe",
                "DEBUG-c1080 phase=decoded-frame-callback-end au=%llu",
                static_cast<unsigned long long>(probe_frame_index));
        }
    }

    av_frame_free(&callback_frame);
    return true;
}

bool VideoDecoder::decode(const uint8_t* data, size_t len, uint64_t timestamp) {
    if (!initialized_) return false;
    auto* ctx = static_cast<AVCodecContext*>(codec_ctx_);
    const int log_index = g_video_decode_logs.fetch_add(1);
    const uint64_t probe_frame_index = static_cast<uint64_t>(log_index) + 1;
    const bool log = shouldLogVideoDecode(log_index);
    const auto au = inspectH264AccessUnit(data, len);
    if (lunar::shouldSampleCloud1080CrashProbe(probe_frame_index)) {
        lunar::cloud1080CrashProbeLog(
            "crash-probe",
            "DEBUG-c1080 phase=decode-begin au=%llu bytes=%zu pts_ns=%llu "
            "nal=%s sps=%d pps=%d idr=%d ready=%d",
            static_cast<unsigned long long>(probe_frame_index),
            len,
            static_cast<unsigned long long>(timestamp),
            au.nal_types.empty() ? "-" : au.nal_types.c_str(),
            au.has_sps ? 1 : 0,
            au.has_pps ? 1 : 0,
            au.has_idr ? 1 : 0,
            h264_decoder_ready_ ? 1 : 0);
    }
    if (perf_) {
        perf_->recordVideoAccessUnit(
            len,
            timestamp,
            perf_->lastVideoAccessUnitQueueAgeUs(),
            au.has_idr);
    }
    if (log) {
        lunar::diagnosticLog("video",
                             "video decode begin index=%d len=%zu ts=%llu nal=%s sps=%d pps=%d idr=%d ready=%d",
                             log_index,
                             len,
                             static_cast<unsigned long long>(timestamp),
                             au.nal_types.empty() ? "-" : au.nal_types.c_str(),
                             au.has_sps ? 1 : 0,
                             au.has_pps ? 1 : 0,
                             au.has_idr ? 1 : 0,
                             h264_decoder_ready_ ? 1 : 0);
    }

    h264_seen_sps_ = h264_seen_sps_ || au.has_sps;
    h264_seen_pps_ = h264_seen_pps_ || au.has_pps;
    // Chiaki reports the PS5's initial SPS/PPS as a standalone access unit.
    // NVDEC rejects parameter-set-only packets, so retain them and prepend
    // them to the next VCL access unit instead of treating this as a decode
    // failure. The same path handles standalone parameter-set updates.
    if (!au.has_vcl) {
        constexpr size_t kMaxH264ParameterSetBytes = 64 * 1024;
        if (au.has_sps) h264_parameter_sets_.clear();
        if (au.has_sps || au.has_pps) {
            if (len <= kMaxH264ParameterSetBytes &&
                h264_parameter_sets_.size() <=
                    kMaxH264ParameterSetBytes - len) {
                h264_parameter_sets_.insert(h264_parameter_sets_.end(),
                                            data, data + len);
                h264_parameter_sets_pending_ = true;
            } else {
                h264_parameter_sets_.clear();
                h264_parameter_sets_pending_ = false;
                lunar::diagnosticLog(
                    "video", "discard oversized H264 parameter sets len=%zu", len);
            }
        }
        if (log) {
            lunar::diagnosticLog(
                "video", "consume H264 non-VCL access unit nal=%s len=%zu cached=%zu",
                au.nal_types.empty() ? "-" : au.nal_types.c_str(),
                len, h264_parameter_sets_.size());
        }
        return true;
    }

    if (!h264_decoder_ready_) {
        if (h264_seen_sps_ && h264_seen_pps_ && au.has_idr) {
            h264_decoder_ready_ = true;
            lunar::diagnosticLog("video",
                                 "H264 decoder gate opened nal=%s len=%zu",
                                 au.nal_types.empty() ? "-" : au.nal_types.c_str(),
                                 len);
        } else if (au.has_vcl) {
            if (h264_wait_log_count_++ < kH264WaitLogLimit) {
                lunar::diagnosticLog("video",
                                     "drop H264 until SPS/PPS/IDR nal=%s len=%zu sps=%d pps=%d idr=%d",
                                     au.nal_types.empty() ? "-" : au.nal_types.c_str(),
                                     len,
                                     h264_seen_sps_ ? 1 : 0,
                                     h264_seen_pps_ ? 1 : 0,
                                     au.has_idr ? 1 : 0);
            }
            return true;
        }
    }

    std::vector<uint8_t> startup_access_unit;
    if (h264_parameter_sets_pending_ && !h264_parameter_sets_.empty()) {
        startup_access_unit.reserve(h264_parameter_sets_.size() + len);
        startup_access_unit.insert(startup_access_unit.end(),
                                   h264_parameter_sets_.begin(),
                                   h264_parameter_sets_.end());
        startup_access_unit.insert(startup_access_unit.end(), data, data + len);
        data = startup_access_unit.data();
        len = startup_access_unit.size();
        h264_parameter_sets_pending_ = false;
        lunar::diagnosticLog("video",
                             "prepended cached H264 parameter sets bytes=%zu total=%zu",
                             h264_parameter_sets_.size(), len);
    }

#ifdef __SWITCH__
    if (usesHardwareDecode(video_backend_)) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            if (perf_) perf_->recordVideoDecodeError();
            logDecodeDrop(perf_, "hardware_packet_alloc", 0, 0,
                          timestamp, len, &au, ctx->width, ctx->height);
            lunar::diagnosticLog("video", "hardware packet alloc failed index=%d", log_index);
            return false;
        }
        int packet_ret = av_new_packet(pkt, static_cast<int>(len));
        if (packet_ret < 0) {
            if (perf_) perf_->recordVideoDecodeError();
            logDecodeDrop(perf_, "hardware_packet_buffer_alloc", packet_ret, 0,
                          timestamp, len, &au, ctx->width, ctx->height);
            logVideoDecodeError("hardware av_new_packet failed",
                                packet_ret,
                                h264_error_log_count_);
            av_packet_free(&pkt);
            return false;
        }
        std::memcpy(pkt->data, data, len);
        if (au.has_idr) pkt->flags |= AV_PKT_FLAG_KEY;

        int ret = avcodec_send_packet(ctx, pkt);
        if (lunar::shouldSampleCloud1080CrashProbe(probe_frame_index)) {
            lunar::cloud1080CrashProbeLog(
                "crash-probe",
                "DEBUG-c1080 phase=decode-send au=%llu ret=%d",
                static_cast<unsigned long long>(probe_frame_index), ret);
        }
        bool decode_ok = ret >= 0 || ret == AVERROR(EAGAIN);
        if (log) {
            lunar::diagnosticLog("video",
                                 "hardware avcodec_send_packet ret=%d index=%d",
                                 ret,
                                 log_index);
        }

        int decoded_frames = 0;
        auto receive_available = [&]() {
            int receive_ret = 0;
            while (receive_ret >= 0) {
                AVFrame* frame = av_frame_alloc();
                if (!frame) {
                    decode_ok = false;
                    logDecodeDrop(perf_, "hardware_receive_frame_alloc", 0, 0,
                                  timestamp, len, &au, ctx->width, ctx->height);
                    break;
                }
                auto t0 = std::chrono::high_resolution_clock::now();
                receive_ret = avcodec_receive_frame(ctx, frame);
                auto t1 = std::chrono::high_resolution_clock::now();
                if (receive_ret == 0) {
                    decoded_frames++;
                    if (log) {
                        lunar::diagnosticLog("video",
                                             "hardware avcodec_receive_frame frame index=%d width=%d height=%d format=%d",
                                             log_index,
                                             frame->width,
                                             frame->height,
                                             frame->format);
                    }
                    if (perf_) {
                        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        perf_->recordDecodeLatencyNs(static_cast<uint64_t>(ns));
                    }
                    if (!deliverFrame(frame, timestamp, log_index, log)) {
                        decode_ok = false;
                    }
                } else {
                    if (log && receive_ret != AVERROR(EAGAIN) && receive_ret != AVERROR_EOF) {
                        logVideoDecodeError("hardware avcodec_receive_frame failed",
                                            receive_ret,
                                            h264_error_log_count_);
                    }
                    if (receive_ret != AVERROR(EAGAIN) && receive_ret != AVERROR_EOF) {
                        logDecodeDrop(perf_, "hardware_receive_frame_failed",
                                      receive_ret, 0, timestamp, len, &au,
                                      ctx->width, ctx->height);
                        decode_ok = false;
                    }
                    av_frame_free(&frame);
                }
            }
        };

        if (ret == AVERROR(EAGAIN)) {
            const int before_retry_frames = decoded_frames;
            receive_available();
            const int retry_ret = avcodec_send_packet(ctx, pkt);
            if (log || retry_ret < 0) {
                lunar::diagnosticLog("video",
                                     "hardware avcodec_send_packet retry ret=%d index=%d drained=%d",
                                     retry_ret,
                                     log_index,
                                     decoded_frames - before_retry_frames);
            }
            ret = retry_ret;
            if (retry_ret < 0 && retry_ret != AVERROR(EAGAIN)) {
                decode_ok = false;
            }
        } else if (ret == AVERROR_UNKNOWN && h264_error_log_count_ < kVideoErrorLogLimit) {
            lunar::diagnosticLog("video",
                                 "NVDEC status error; packet not retried index=%d len=%zu ts=%llu",
                                 log_index,
                                 len,
                                 static_cast<unsigned long long>(timestamp));
        }

        av_packet_free(&pkt);
        receive_available();
        if (ret < 0 && decoded_frames == 0) {
            if (perf_) perf_->recordVideoDecodeError();
            logDecodeDrop(perf_, "hardware_send_packet_rejected", ret, 0,
                          timestamp, len, &au, ctx->width, ctx->height);
            if (h264_error_log_count_ < kVideoErrorLogLimit) {
                lunar::diagnosticLog("video",
                                     "hardware avcodec_send_packet rejected index=%d len=%zu ts=%llu nal=%s sps=%d pps=%d idr=%d",
                                     log_index,
                                     len,
                                     static_cast<unsigned long long>(timestamp),
                                     au.nal_types.empty() ? "-" : au.nal_types.c_str(),
                                     au.has_sps ? 1 : 0,
                                     au.has_pps ? 1 : 0,
                                     au.has_idr ? 1 : 0);
            }
            logVideoDecodeError("hardware avcodec_send_packet failed",
                                ret,
                                h264_error_log_count_);
            decode_ok = false;
        }
        if (log) {
            lunar::diagnosticLog("video",
                                 "hardware video decode done index=%d frames=%d send_ret=%d",
                                 log_index,
                                 decoded_frames,
                                 ret);
        }
        if (lunar::shouldSampleCloud1080CrashProbe(probe_frame_index)) {
            lunar::cloud1080CrashProbeLog(
                "crash-probe",
                "DEBUG-c1080 phase=decode-end au=%llu frames=%d send_ret=%d ok=%d",
                static_cast<unsigned long long>(probe_frame_index),
                decoded_frames,
                ret,
                decode_ok ? 1 : 0);
        }
        return decode_ok;
    }
#endif

    // =========================================================================
    // Parse complete RTP-depacketized H.264 access units into decoder-ready
    // packets. Moonlight submits complete decode units; LunarNX now mirrors
    // that boundary at the RTP layer and keeps FFmpeg's parser as a guardrail.
    // =========================================================================
    auto* parser = static_cast<AVCodecParserContext*>(parser_);
    if (!parser) return false;
    const uint8_t* p_data = data;
    int p_size = static_cast<int>(len);

    bool decode_ok = true;
    while (p_size > 0) {
        uint8_t* out_data = nullptr;
        int out_size = 0;

        int parsed = av_parser_parse2(parser, ctx, &out_data, &out_size,
                                       p_data, p_size, 0, 0, 0);
        if (parsed < 0) {
            if (perf_) perf_->recordVideoDecodeError();
            logDecodeDrop(perf_, "software_parser_failed", parsed, 0,
                          timestamp, len, &au, ctx->width, ctx->height);
            logVideoDecodeError("av_parser_parse2 failed", parsed, h264_error_log_count_);
            return false;
        }
        if (parsed == 0 && out_size == 0) {
            break;
        }
        p_data += parsed;
        p_size -= parsed;

        if (out_size > 0) {
            AVPacket* pkt = av_packet_alloc();
            if (!pkt) {
                decode_ok = false;
                continue;
            }
            pkt->data = out_data;
            pkt->size = out_size;

            int ret = avcodec_send_packet(ctx, pkt);
            av_packet_free(&pkt);
            if (log) {
                lunar::diagnosticLog("video",
                                     "avcodec_send_packet ret=%d index=%d",
                                     ret,
                                     log_index);
            }
            if (ret < 0) {
                decode_ok = false;
                if (perf_) perf_->recordVideoDecodeError();
                logDecodeDrop(perf_, "software_send_packet_rejected", ret, 0,
                              timestamp, len, &au, ctx->width, ctx->height);
                if (h264_error_log_count_ < kVideoErrorLogLimit) {
                    lunar::diagnosticLog("video",
                                         "avcodec_send_packet rejected index=%d len=%zu ts=%llu nal=%s sps=%d pps=%d idr=%d",
                                         log_index,
                                         len,
                                         static_cast<unsigned long long>(timestamp),
                                         au.nal_types.empty() ? "-" : au.nal_types.c_str(),
                                         au.has_sps ? 1 : 0,
                                         au.has_pps ? 1 : 0,
                                         au.has_idr ? 1 : 0);
                }
                logVideoDecodeError("avcodec_send_packet failed", ret, h264_error_log_count_);
                continue;
            }

            int decoded_frames = 0;
            while (ret >= 0) {
                AVFrame* frame = av_frame_alloc();
                if (!frame) break;
                auto t0 = std::chrono::high_resolution_clock::now();
                ret = avcodec_receive_frame(ctx, frame);
                auto t1 = std::chrono::high_resolution_clock::now();

                if (ret == 0) {
                    decoded_frames++;
                    if (log) {
                        lunar::diagnosticLog("video",
                                             "avcodec_receive_frame frame index=%d width=%d height=%d format=%d",
                                             log_index,
                                             frame->width,
                                             frame->height,
                                             frame->format);
                    }
                    if (perf_) {
                        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        perf_->recordDecodeLatencyNs(static_cast<uint64_t>(ns));
                    }
                    if (!deliverFrame(frame, timestamp, log_index, log)) {
                        decode_ok = false;
                    }
                } else {
                    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                        if (log && perf_) perf_->recordVideoDecodeError();
                        logDecodeDrop(perf_, "software_receive_frame_failed", ret, 0,
                                      timestamp, len, &au,
                                      ctx->width, ctx->height);
                    }
                    if (log && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                        logVideoDecodeError("avcodec_receive_frame failed", ret, h264_error_log_count_);
                    }
                    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                        decode_ok = false;
                    }
                    av_frame_free(&frame);
                }
            }
            if (log) {
                lunar::diagnosticLog("video",
                                     "video decode done index=%d frames=%d last_ret=%d",
                                     log_index,
                                     decoded_frames,
                                     ret);
            }
        }
    }
    return decode_ok;
}

void VideoDecoder::setCallback(FrameCallback cb) { on_frame_ = std::move(cb); }

bool VideoDecoder::resetForKeyframe() {
    if (!initialized_ || !codec_ctx_) return false;

    auto* ctx = static_cast<AVCodecContext*>(codec_ctx_);
    avcodec_flush_buffers(ctx);

    if (parser_) {
        av_parser_close(static_cast<AVCodecParserContext*>(parser_));
        parser_ = nullptr;
    }
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    if (!parser) {
        lunar::diagnosticLog("video", "H264 parser reinit failed during keyframe reset");
        h264_decoder_ready_ = false;
        return false;
    }
    parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    parser_ = parser;
    // Keep SPS/PPS knowledge: Xbox may send them only in the first IDR, while
    // a requested recovery IDR can be a slice-only access unit.
    h264_decoder_ready_ = false;
    h264_parameter_sets_pending_ = !h264_parameter_sets_.empty();
    h264_wait_log_count_ = 0;
    lunar::diagnosticLog("video", "H264 decoder reset; waiting for IDR");
    return true;
}

void VideoDecoder::flush() {
    if (!initialized_) return;
    auto* ctx = static_cast<AVCodecContext*>(codec_ctx_);

    avcodec_send_packet(ctx, nullptr);

    while (true) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) break;
        int ret = avcodec_receive_frame(ctx, frame);
        if (ret == 0) {
            deliverFrame(frame, 0, -1, false);
        } else {
            av_frame_free(&frame);
        }
        if (ret < 0) break;
    }
}

void VideoDecoder::shutdown() {
    if (parser_) {
        av_parser_close(static_cast<AVCodecParserContext*>(parser_));
        parser_ = nullptr;
    }
#ifdef __SWITCH__
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
#endif
    if (codec_ctx_) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&codec_ctx_));
    }
    h264_decoder_ready_ = false;
    h264_seen_sps_ = false;
    h264_seen_pps_ = false;
    h264_parameter_sets_.clear();
    h264_parameter_sets_pending_ = false;
    h264_wait_log_count_ = 0;
    h264_error_log_count_ = 0;
    initialized_ = false;
}

} // namespace lunar::stream
