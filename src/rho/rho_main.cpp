/**
 * rho — Local video player.
 * Video: FFmpeg NVDEC hw decode (copy-out) or software decode + nanovg display
 * Audio: FFmpeg decode + swresample + AudioPlayer (audren on Switch)
 *
 * Experiment: kRhoUseNvdec=true uses NVDEC decode with av_hwframe_transfer_data
 * to copy NVDEC output to CPU NV12, then sws_scale→RGBA→nanovg.
 */

#include "../common.h"
#include "../diagnostics.h"
#include "../stream/audio_player.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __SWITCH__
#include <borealis.hpp>
#include <deko3d.hpp>
#include <borealis/platforms/switch/switch_video.hpp>
#include <nanovg/framework/CMemPool.h>
#include <nanovg/framework/CShader.h>
#include <nanovg/framework/CCmdMemRing.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#ifdef __SWITCH__
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_nvtegra.h>
#endif
}

// Experiment toggle: true = NVDEC hw decode with copy-out (Experiment 1)
#ifdef __SWITCH__
static constexpr bool kRhoUseNvdec = true;
static constexpr bool kRhoUseDeko3d = false;
#else
static constexpr bool kRhoUseNvdec = false;
static constexpr bool kRhoUseDeko3d = false;
#endif

namespace lunar::rho {
namespace {

const char* defaultVideoPath() {
#ifdef __SWITCH__
    return "sdmc:/test_stream_pcm.mp4";
#else
    return "/tmp/test_stream.mp4";
#endif
}

// =========================================================================
// Demuxer
// =========================================================================
struct Demuxer {
    AVFormatContext* fmt = nullptr;
    AVIOContext* avio = nullptr;
    uint8_t* io_buffer = nullptr;
    uint8_t* file_data = nullptr;
    size_t file_size = 0, file_pos = 0;
    int video_idx = -1, audio_idx = -1;
    bool eof = false;

    static int readPkt(void* opaque, uint8_t* buf, int sz) {
        auto* d = (Demuxer*)opaque;
        size_t r = d->file_size - d->file_pos;
        if (!r) return AVERROR_EOF;
        size_t n = std::min((size_t)sz, r);
        memcpy(buf, d->file_data + d->file_pos, n);
        d->file_pos += n;
        return (int)n;
    }
    static int64_t seekPkt(void* opaque, int64_t off, int whence) {
        auto* d = (Demuxer*)opaque;
        int64_t np;
        switch (whence) {
            case SEEK_SET: np = off; break;
            case SEEK_CUR: np = (int64_t)d->file_pos + off; break;
            case SEEK_END: np = (int64_t)d->file_size + off; break;
            case AVSEEK_SIZE: return (int64_t)d->file_size;
            default: return -1;
        }
        if (np < 0) np = 0;
        if (np > (int64_t)d->file_size) np = (int64_t)d->file_size;
        d->file_pos = (size_t)np;
        return np;
    }

    bool open(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END); file_size = ftell(f); fseek(f, 0, SEEK_SET);
        file_data = (uint8_t*)av_malloc(file_size);
        if (!file_data || fread(file_data, 1, file_size, f) != file_size) { fclose(f); return false; }
        fclose(f);
        lunar::diagnosticLog("rho", "read %zu bytes", file_size);

        io_buffer = (uint8_t*)av_malloc(32768);
        avio = avio_alloc_context(io_buffer, 32768, 0, this, readPkt, nullptr, seekPkt);
        fmt = avformat_alloc_context(); fmt->pb = avio;
        if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) { close(); return false; }

        video_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        audio_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

        auto* vp = fmt->streams[video_idx]->codecpar;
        if (audio_idx >= 0) {
            auto* ap = fmt->streams[audio_idx]->codecpar;
            lunar::diagnosticLog("rho", "video %dx%d codec=%d audio=%dHz/%dch codec=%d",
                vp->width, vp->height, vp->codec_id,
                ap->sample_rate, ap->ch_layout.nb_channels, ap->codec_id);
        } else {
            lunar::diagnosticLog("rho", "video %dx%d codec=%d (no audio)",
                vp->width, vp->height, vp->codec_id);
        }
        return true;
    }

    int videoW() const { return fmt->streams[video_idx]->codecpar->width; }
    int videoH() const { return fmt->streams[video_idx]->codecpar->height; }
    double videoFps() const {
        AVRational rate = fmt->streams[video_idx]->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) rate = fmt->streams[video_idx]->r_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) return 30.0;
        return static_cast<double>(rate.num) / static_cast<double>(rate.den);
    }
    AVCodecParameters* videoPar() const { return fmt->streams[video_idx]->codecpar; }
    AVCodecParameters* audioPar() const {
        return audio_idx >= 0 ? fmt->streams[audio_idx]->codecpar : nullptr;
    }

    bool readNext(AVPacket** vout, AVPacket** aout) {
        *vout = *aout = nullptr;
        while (!eof) {
            AVPacket* p = av_packet_alloc();
            if (!p) break;
            int r = av_read_frame(fmt, p);
            if (r < 0) { av_packet_free(&p); if (r == AVERROR_EOF) eof = true; return false; }
            if (p->stream_index == video_idx) { *vout = p; return true; }
            if (p->stream_index == audio_idx) { *aout = p; return true; }
            av_packet_free(&p);
        }
        return false;
    }

    void seekToStart() {
        eof = false; file_pos = 0;
        av_seek_frame(fmt, video_idx, 0, AVSEEK_FLAG_BACKWARD);
        if (audio_idx >= 0) av_seek_frame(fmt, audio_idx, 0, AVSEEK_FLAG_BACKWARD);
    }

    void close() {
        if (fmt) { fmt->pb = nullptr; avformat_close_input(&fmt); }
        if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); avio = nullptr; io_buffer = nullptr; }
        if (file_data) { av_free(file_data); file_data = nullptr; }
    }
};

// =========================================================================
// Software FFmpeg decoder
// =========================================================================
struct SwDecoder {
    AVCodecContext* ctx = nullptr;

    bool init(AVCodecParameters* par) {
        const AVCodec* c = avcodec_find_decoder(par->codec_id);
        if (!c) return false;
        ctx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(ctx, par);
        return avcodec_open2(ctx, c, nullptr) >= 0;
    }

    bool decode(AVPacket* pkt, std::vector<AVFrame*>& frames) {
        if (avcodec_send_packet(ctx, pkt) < 0) return false;
        while (true) {
            AVFrame* f = av_frame_alloc();
            int r = avcodec_receive_frame(ctx, f);
            if (r < 0) { av_frame_free(&f); return r == AVERROR(EAGAIN) || r == AVERROR_EOF; }
            frames.push_back(f);
        }
    }

    void drain(std::vector<AVFrame*>& frames) {
        avcodec_send_packet(ctx, nullptr);
        while (true) {
            AVFrame* f = av_frame_alloc();
            if (avcodec_receive_frame(ctx, f) < 0) { av_frame_free(&f); break; }
            frames.push_back(f);
        }
    }

    void shutdown() { if (ctx) { avcodec_free_context(&ctx); } }
};

#ifdef __SWITCH__
// =========================================================================
// NVDEC hardware decoder with copy-out (av_hwframe_transfer_data)
// Uses NVDEC for decode but copies output to CPU NV12, avoiding the
// zero-copy NvMap->deko3d external storage path that fails on Ryujinx.
// =========================================================================
struct NvdecDecoder {
    AVBufferRef* hw_device_ctx = nullptr;
    AVCodecContext* ctx = nullptr;
    bool ok = false;
    int frame_count = 0;
    int send_error_count = 0;
    int no_output_count = 0;

    bool init(AVCodecParameters* par) {
        int err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_NVTEGRA,
                                          nullptr, nullptr, 0);
        if (err < 0) {
            lunar::diagnosticLog("rho-nvdec", "hwdevice_ctx_create failed err=%d", err);
            return false;
        }
        lunar::diagnosticLog("rho-nvdec", "hwdevice_ctx_create done");

        const AVCodec* c = avcodec_find_decoder(par->codec_id);
        if (!c) { lunar::diagnosticLog("rho-nvdec", "decoder not found"); return false; }

        ctx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(ctx, par);
        ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        ctx->pix_fmt = AV_PIX_FMT_NVTEGRA;
        ctx->thread_count = 1;

        // Increase hw frame pool size (default may be too small for Ryujinx)
        ctx->extra_hw_frames = 16;

        err = avcodec_open2(ctx, c, nullptr);
        if (err < 0) {
            lunar::diagnosticLog("rho-nvdec", "avcodec_open2 failed err=%d", err);
            return false;
        }
        lunar::diagnosticLog("rho-nvdec", "avcodec_open2 done pix_fmt=%d extra_hw=%d",
            ctx->pix_fmt, ctx->extra_hw_frames);

        ok = true;
        frame_count = 0;
        send_error_count = 0;
        no_output_count = 0;
        return true;
    }

    bool decode(AVPacket* pkt, std::vector<AVFrame*>& out_frames) {
        if (!ok) return false;
        int ret = avcodec_send_packet(ctx, pkt);
        if (ret < 0) {
            const int send_ret = ret;
            if (send_error_count++ < 20) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
                av_strerror(ret, errbuf, sizeof(errbuf));
                lunar::diagnosticLog("rho-nvdec",
                    "send_packet failed ret=%d %s size=%d pts=%lld dts=%lld flags=0x%x",
                    ret,
                    errbuf,
                    pkt ? pkt->size : 0,
                    pkt ? (long long)pkt->pts : -1LL,
                    pkt ? (long long)pkt->dts : -1LL,
                    pkt ? pkt->flags : 0);
            }
            // Drain pending frames even when send fails (queued frames from previous sends)
            while (true) {
                AVFrame* hw_f = av_frame_alloc();
                ret = avcodec_receive_frame(ctx, hw_f);
                if (ret < 0) { av_frame_free(&hw_f); break; }
                frame_count++;
                AVFrame* sw_f = av_frame_alloc();
                if (av_hwframe_transfer_data(sw_f, hw_f, 0) >= 0) {
                    av_frame_copy_props(sw_f, hw_f);
                    out_frames.push_back(sw_f);
                } else { av_frame_free(&sw_f); }
                av_frame_free(&hw_f);
            }
            if (send_ret != AVERROR(EAGAIN)) {
                return !out_frames.empty();
            }

            ret = avcodec_send_packet(ctx, pkt);
            if (ret < 0) {
                if (send_error_count++ < 20) {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    lunar::diagnosticLog("rho-nvdec",
                        "send_packet retry failed ret=%d %s size=%d pts=%lld dts=%lld flags=0x%x",
                        ret,
                        errbuf,
                        pkt ? pkt->size : 0,
                        pkt ? (long long)pkt->pts : -1LL,
                        pkt ? (long long)pkt->dts : -1LL,
                        pkt ? pkt->flags : 0);
                }
                return !out_frames.empty();
            }
            if (send_error_count <= 20) {
                lunar::diagnosticLog("rho-nvdec",
                    "send_packet retry ok size=%d pts=%lld dts=%lld flags=0x%x",
                    pkt ? pkt->size : 0,
                    pkt ? (long long)pkt->pts : -1LL,
                    pkt ? (long long)pkt->dts : -1LL,
                    pkt ? pkt->flags : 0);
            }
        }
        while (ret >= 0) {
            AVFrame* hw_f = av_frame_alloc();
            ret = avcodec_receive_frame(ctx, hw_f);
            if (ret < 0) { av_frame_free(&hw_f); break; }
            frame_count++;
            AVFrame* sw_f = av_frame_alloc();
            if (av_hwframe_transfer_data(sw_f, hw_f, 0) >= 0) {
                av_frame_copy_props(sw_f, hw_f);
                if (frame_count <= 5) {
                    lunar::diagnosticLog("rho-nvdec", "frame %d: w=%d h=%d fmt=%d",
                        frame_count, sw_f->width, sw_f->height, sw_f->format);
                }
                out_frames.push_back(sw_f);
            } else { av_frame_free(&sw_f); }
            av_frame_free(&hw_f);
        }
        if (out_frames.empty() && no_output_count++ < 20) {
            lunar::diagnosticLog("rho-nvdec",
                "send_packet produced no frame size=%d pts=%lld dts=%lld flags=0x%x",
                pkt ? pkt->size : 0,
                pkt ? (long long)pkt->pts : -1LL,
                pkt ? (long long)pkt->dts : -1LL,
                pkt ? pkt->flags : 0);
        }
        return true;
    }
    void drain(std::vector<AVFrame*>& out_frames) {
        if (!ok) return;
        avcodec_send_packet(ctx, nullptr);
        while (true) {
            AVFrame* hw_f = av_frame_alloc();
            int ret = avcodec_receive_frame(ctx, hw_f);
            if (ret < 0) { av_frame_free(&hw_f); break; }
            AVFrame* sw_f = av_frame_alloc();
            if (av_hwframe_transfer_data(sw_f, hw_f, 0) >= 0) {
                out_frames.push_back(sw_f);
            } else {
                av_frame_free(&sw_f);
            }
            av_frame_free(&hw_f);
        }
    }

    void shutdown() {
        ok = false;
        if (ctx) { avcodec_free_context(&ctx); ctx = nullptr; }
        if (hw_device_ctx) { av_buffer_unref(&hw_device_ctx); hw_device_ctx = nullptr; }
    }
};

// =========================================================================
// deko3d NV12->RGB renderer (no UsageVideo, no external storage)
// Uploads NV12 via pitch-linear staging + 2D engine copy to block-linear
// =========================================================================
struct Deko3DRenderer {
    static constexpr unsigned kUpdateSlice = 0x4000;
    static constexpr unsigned kRenderSlice = 0x8000;

    struct Vertex { float pos[3]; float uv[2]; };
    const std::array<DkVtxAttribState, 2> kAttribs = {{
        DkVtxAttribState{0, 0, offsetof(Vertex, pos), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(Vertex, uv),  DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
    }};
    const std::array<DkVtxBufferState, 1> kBufState = {{{sizeof(Vertex), 0}}};
    static constexpr Vertex kQuad[4] = {
        {{-1, 1, 0}, {0, 0}}, {{-1, -1, 0}, {0, 1}},
        {{1, -1, 0}, {1, 1}}, {{1, 1, 0}, {1, 0}},
    };

    struct Tf { alignas(16) float c0[4], c1[4], c2[4]; alignas(16) float o[4]; alignas(16) float u[4]; };

    brls::SwitchVideoContext* vctx = nullptr;
    dk::UniqueDevice dev{};
    ::DkQueue queue = nullptr;
    dk::UniqueCmdBuf update_cb{}, render_cb{};
    std::optional<CMemPool> pc, pd, pi;
    std::optional<CCmdMemRing<2>> update_ring, render_ring;
    CShader vshader, fshader;
    CMemPool::Handle vb_handle{};
    CMemPool::Handle tf_handle{};
    int luma_slot = -1, chroma_slot = -1;
    int frame_w = 0, frame_h = 0;
    // Persistent per-frame allocations (reused across frames)
    CMemPool::Handle staging_handle{};
    CMemPool::Handle luma_img_handle{};
    CMemPool::Handle chroma_img_handle{};
    dk::Image luma_img{}, chroma_img{};
    bool images_ready = false;
    int target_w = 1920, target_h = 1080;
    bool ok = false;
    std::mutex mutex;
    dk::Fence render_fence;

    ~Deko3DRenderer() { shutdown(); }

    bool initialize() {
        auto* plat = brls::Application::getPlatform();
        if (!plat) { lunar::diagnosticLog("rho-dk", "no platform"); return false; }
        vctx = static_cast<brls::SwitchVideoContext*>(plat->getVideoContext());
        if (!vctx) { lunar::diagnosticLog("rho-dk", "no video ctx"); return false; }
        dev = dk::UniqueDevice{vctx->getDeko3dDevice()};
        queue = vctx->getQueue();
        if (!dev || !queue) { lunar::diagnosticLog("rho-dk", "no dev/queue"); return false; }

        try {
            pc.emplace(dev, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, 128 * 1024);
            pd.emplace(dev, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 4 * 1024 * 1024);
            pi.emplace(dev, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
            update_cb = dk::CmdBufMaker{dev}.create();
            render_cb = dk::CmdBufMaker{dev}.create();
            update_ring.emplace(); render_ring.emplace();
            if (!update_ring->allocate(*pd, kUpdateSlice) || !render_ring->allocate(*pd, kRenderSlice)) {
                lunar::diagnosticLog("rho-dk", "ring alloc fail"); return false;
            }

            vshader.load(*pc, "romfs:/shaders/basic_vsh.dksh");
            fshader.load(*pc, "romfs:/shaders/texture_fsh.dksh");
            if (!vshader || !fshader) { lunar::diagnosticLog("rho-dk", "shader fail"); return false; }
            lunar::diagnosticLog("rho-dk", "shaders loaded nv12=%s", fshader ? "ok" : "fail");

            vb_handle = pd->allocate(sizeof(kQuad), alignof(Vertex));
            if (!vb_handle) { lunar::diagnosticLog("rho-dk", "vb fail"); return false; }
            memcpy(vb_handle.getCpuAddr(), kQuad, sizeof(kQuad));

            tf_handle = pd->allocate(sizeof(Tf), DK_UNIFORM_BUF_ALIGNMENT);
            if (!tf_handle) { lunar::diagnosticLog("rho-dk", "tf fail"); return false; }

            luma_slot = vctx->allocateImageIndex();
            chroma_slot = vctx->allocateImageIndex();
            if (luma_slot < 0 || chroma_slot < 0) { lunar::diagnosticLog("rho-dk", "slot fail"); return false; }

            target_w = brls::Application::windowWidth > 0 ? (int)brls::Application::windowWidth : 1920;
            target_h = brls::Application::windowHeight > 0 ? (int)brls::Application::windowHeight : 1080;

            ok = true;
            lunar::diagnosticLog("rho-dk", "init ok %dx%d", target_w, target_h);
        } catch (...) {
            lunar::diagnosticLog("rho-dk", "init exception");
            return false;
        }
        return true;
    }

    static Tf nv12Transform(int w, int h) {
        // BT.709 limited range YCbCr -> RGB
        Tf t{
            {1.1644f, 1.1644f, 1.1644f, 0},
            {0, -0.2132f, 2.1124f, 0},
            {1.7927f, -0.5329f, 0, 0},
            {16.0f/255.0f, 128.0f/255.0f, 128.0f/255.0f, 0},
            {0, 0, 1.0f, 1.0f}
        };
        return t;
    }

    static void saveFirstFrame(const uint8_t* nv12[2], const int ls[2], int w, int h) {
        static int count = 0;
        if (count++ > 0) return;
        FILE* f = fopen("sdmc:/rho_deko3d_nv12.raw", "wb");
        if (!f) return;
        uint32_t hdr[4] = {(uint32_t)w, (uint32_t)h, (uint32_t)ls[0], (uint32_t)ls[1]};
        fwrite(hdr, 4, 4, f);
        for (int y = 0; y < h; y++) fwrite(nv12[0] + y * ls[0], 1, w, f);
        for (int y = 0; y < h/2; y++) fwrite(nv12[1] + y * ls[1], 1, w, f);
        fclose(f);
        lunar::diagnosticLog("rho-dk", "saved NV12 frame %dx%d to SD", w, h);
    }

    bool uploadNV12(const uint8_t* nv12[2], const int linesize[2], int w, int h) {
        if (!ok) return false;
        saveFirstFrame(nv12, linesize, w, h);
        std::lock_guard<std::mutex> lock(mutex);

        size_t luma_sz = (size_t)linesize[0] * h;
        size_t chroma_sz = (size_t)linesize[1] * h / 2;
        size_t total = luma_sz + chroma_sz;

        // Allocate persistent buffers on first frame (or if size changes)
        if (!images_ready || frame_w != w || frame_h != h) {
            staging_handle.destroy();
            luma_img_handle.destroy();
            chroma_img_handle.destroy();

            staging_handle = pd->allocate(total, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
            if (!staging_handle) return false;

            dk::ImageLayout ll, cl;
            dk::ImageLayoutMaker{dev}
                .setType(DkImageType_2D).setFormat(DkImageFormat_R8_Unorm)
                .setDimensions(w, h, 1)
                .setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine)
                .initialize(ll);
            dk::ImageLayoutMaker{dev}
                .setType(DkImageType_2D).setFormat(DkImageFormat_RG8_Unorm)
                .setDimensions(w/2, h/2, 1)
                .setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine)
                .initialize(cl);

            luma_img_handle = pi->allocate(ll.getSize(), ll.getAlignment());
            chroma_img_handle = pi->allocate(cl.getSize(), cl.getAlignment());
            if (!luma_img_handle || !chroma_img_handle) return false;

            luma_img.initialize(ll, luma_img_handle.getMemBlock(), luma_img_handle.getOffset());
            chroma_img.initialize(cl, chroma_img_handle.getMemBlock(), chroma_img_handle.getOffset());
            images_ready = true;
            frame_w = w; frame_h = h;
            lunar::diagnosticLog("rho-dk", "allocated persistent images %dx%d", w, h);
        }

        // Copy NV12 to staging
        uint8_t* dst = (uint8_t*)staging_handle.getCpuAddr();
        for (int y = 0; y < h; y++) memcpy(dst + y * linesize[0], nv12[0] + y * linesize[0], w);
        for (int y = 0; y < h/2; y++) memcpy(dst + luma_sz + y * linesize[1], nv12[1] + y * linesize[1], w);

        // Upload via 2D engine
        update_ring->begin(update_cb);
        DkGpuAddr gpu = staging_handle.getGpuAddr();

        DkCopyBuf luma_buf{};
        luma_buf.addr = gpu;
        luma_buf.rowLength = (uint32_t)linesize[0];
        luma_buf.imageHeight = (uint32_t)h;
        dk::ImageView lv(luma_img);
        DkImageRect lr{0, 0, 0, (uint32_t)w, (uint32_t)h, 1};
        dkCmdBufCopyBufferToImage(update_cb, &luma_buf, &lv, &lr, 0);

        DkCopyBuf chroma_buf{};
        chroma_buf.addr = gpu + luma_sz;
        chroma_buf.rowLength = (uint32_t)(linesize[1] / 2);
        chroma_buf.imageHeight = (uint32_t)(h / 2);
        dk::ImageView cv(chroma_img);
        DkImageRect cr{0, 0, 0, (uint32_t)(w/2), (uint32_t)(h/2), 1};
        dkCmdBufCopyBufferToImage(update_cb, &chroma_buf, &cv, &cr, 0);

        ::dkQueueSubmitCommands(queue, update_ring->end(update_cb));
        ::dkQueueWaitIdle(queue);

        // Update image descriptors
        dk::ImageDescriptor ld, cd;
        ld.initialize(luma_img);
        cd.initialize(chroma_img);
        update_ring->begin(update_cb);
        vctx->updateImageDescriptor(update_cb, luma_slot, ld);
        vctx->updateImageDescriptor(update_cb, chroma_slot, cd);
        vctx->invalidateImageDescriptors(update_cb);
        ::dkQueueSubmitCommands(queue, update_ring->end(update_cb));

        return true;
    }

    void present() {
        if (!ok || frame_w <= 0) return;
        std::lock_guard<std::mutex> lock(mutex);

        auto* fb = vctx->getFramebuffer();
        auto* db = vctx->getDepthBuffer();
        if (!fb || !db) return;

        // Have borealis flush its work so we render on top
        vctx->queueSignalFence(&render_fence);
        vctx->queueFlush();

        render_ring->begin(render_cb);

        dk::ImageView color{*fb}, depth{*db};
        render_cb.bindRenderTargets(&color, &depth);
        render_cb.setViewports(0, {{{0, 0, (float)target_w, (float)target_h, 0, 1}}});
        render_cb.setScissors(0, {{{0, 0, (uint32_t)target_w, (uint32_t)target_h}}});

        dk::RasterizerState rs;
        dk::DepthStencilState ds;
        dk::ColorState cs;
        dk::ColorWriteState cws;
        render_cb.bindRasterizerState(rs);
        render_cb.bindDepthStencilState(ds.setDepthTestEnable(false).setDepthWriteEnable(false));
        render_cb.bindColorState(cs);
        render_cb.bindColorWriteState(cws);

        render_cb.bindVtxBuffer(0, vb_handle.getGpuAddr(), vb_handle.getSize());
        render_cb.bindVtxAttribState(kAttribs);
        render_cb.bindVtxBufferState(kBufState);

        render_cb.barrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
        render_cb.bindShaders(DkStageFlag_GraphicsMask, {vshader, fshader});
        render_cb.bindTextures(DkStage_Fragment, 0, dkMakeTextureHandle(luma_slot, 0));
        render_cb.bindTextures(DkStage_Fragment, 1, dkMakeTextureHandle(chroma_slot, 0));
        Tf tf = nv12Transform(frame_w, frame_h);
        render_cb.pushConstants(tf_handle.getGpuAddr(), sizeof(Tf), 0, sizeof(Tf), &tf);
        render_cb.draw(DkPrimitive_Quads, 4, 1, 0, 0);

        render_cb.signalFence(static_cast<::DkFence&>(render_fence));
        ::dkQueueSubmitCommands(queue, render_ring->end(render_cb));
    }

    void shutdown() {
        ok = false;
        images_ready = false;
        if (queue) ::dkQueueWaitIdle(queue);
        staging_handle.destroy();
        luma_img_handle.destroy();
        chroma_img_handle.destroy();
        if (vctx) {
            if (luma_slot >= 0) { vctx->freeImageIndex(luma_slot); luma_slot = -1; }
            if (chroma_slot >= 0) { vctx->freeImageIndex(chroma_slot); chroma_slot = -1; }
        }
        tf_handle.destroy();
        vb_handle.destroy();
        render_cb = {};
        update_cb = {};
        render_ring.reset();
        update_ring.reset();
        pi.reset();
        pd.reset();
        pc.reset();
        vctx = nullptr; dev = nullptr; queue = nullptr;
    }
};

#endif // __SWITCH__

// =========================================================================
// Switch: nanovg display + AudioPlayer (audren)
// =========================================================================
#ifdef __SWITCH__

struct Playback {
    Demuxer demuxer;
    SwDecoder video_dec, audio_dec;
    NvdecDecoder nvdec_dec;
    lunar::stream::AudioPlayer audio_player;
    SwrContext* audio_swr = nullptr;
    SwsContext* video_sws = nullptr;
    std::mutex frame_mutex;
    std::vector<uint8_t> rgba_front;
    std::vector<uint8_t> rgba_back;
    int frame_w = 0, frame_h = 0;
    int nvg_image = -1;
    bool has_frame = false;
    Deko3DRenderer deko3d;
    bool deko3d_ready() const { return deko3d.ok; }
    bool use_nvdec = false;
    bool use_deko3d = false; // use deko3d NV12->RGB instead of nanovg
    std::atomic<bool> running{false};
    std::thread worker;
};

class RhoVideoView : public brls::View {
public:
    Playback* pb = nullptr;

    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style s, brls::FrameContext* ctx) override {
        if (pb && pb->use_deko3d) {
            pb->deko3d.present();  // render video on top of borealis
            return;
        }

        nvgBeginPath(vg);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 255));
        nvgRect(vg, x, y, w, h);
        nvgFill(vg);

        if (!pb) return;

        // nanovg fallback
        std::lock_guard<std::mutex> lock(pb->frame_mutex);
        if (pb->has_frame && pb->nvg_image >= 0) {
            nvgUpdateImage(vg, pb->nvg_image, pb->rgba_front.data());
            NVGpaint img = nvgImagePattern(vg, 0, 0, pb->frame_w, pb->frame_h, 0, pb->nvg_image, 1.0f);
            nvgBeginPath(vg); nvgRect(vg, x, y, w, h); nvgFillPaint(vg, img); nvgFill(vg);
        }
    }
};

class RhoActivity : public brls::Activity {
public:
    explicit RhoActivity(const char* p) : path_(p) {}

    brls::View* createContentView() override {
        auto* v = new RhoVideoView();
        v->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
        v->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
        pb_.nvg_image = -1; v->pb = &pb_;
        startPlayback();
        return v;
    }

    ~RhoActivity() { stopPlayback(); }

private:
    std::string path_;
    Playback pb_;

    void startPlayback() {
        if (!pb_.demuxer.open(path_.c_str())) { lunar::diagnosticLog("rho", "demuxer fail"); return; }

        pb_.use_nvdec = kRhoUseNvdec;
        pb_.use_deko3d = kRhoUseDeko3d;
        bool video_ok = false;

        if (pb_.use_deko3d && !pb_.deko3d.initialize()) {
            lunar::diagnosticLog("rho", "deko3d init FAIL (background test)");
        }

        if (pb_.use_nvdec) {
            video_ok = pb_.nvdec_dec.init(pb_.demuxer.videoPar());
            lunar::diagnosticLog("rho", "nvdec init ok=%s", video_ok ? "true" : "false");
            if (!video_ok) {
                // Fallback to software
                pb_.use_nvdec = false;
                video_ok = pb_.video_dec.init(pb_.demuxer.videoPar());
                lunar::diagnosticLog("rho", "nvdec fallback to sw ok=%s", video_ok ? "true" : "false");
            }
        } else {
            video_ok = pb_.video_dec.init(pb_.demuxer.videoPar());
            lunar::diagnosticLog("rho", "sw decode init ok=%s", video_ok ? "true" : "false");
        }
        if (!video_ok) { lunar::diagnosticLog("rho", "video dec fail"); return; }

        pb_.frame_w = pb_.demuxer.videoW(); pb_.frame_h = pb_.demuxer.videoH();
        pb_.rgba_front.assign(pb_.frame_w * pb_.frame_h * 4, 0);
        pb_.rgba_back.assign(pb_.frame_w * pb_.frame_h * 4, 0);

        bool have_audio = false;
        if (auto* ap = pb_.demuxer.audioPar()) {
            if (pb_.audio_dec.init(ap)) {
                auto* actx = pb_.audio_dec.ctx;
                AVChannelLayout in_lay = actx->ch_layout;
                AVChannelLayout out_lay = AV_CHANNEL_LAYOUT_STEREO;
                swr_alloc_set_opts2(&pb_.audio_swr,
                    &out_lay, AV_SAMPLE_FMT_S16, 48000,
                    &in_lay, actx->sample_fmt, actx->sample_rate, 0, nullptr);
                if (pb_.audio_swr && swr_init(pb_.audio_swr) >= 0) {
                    have_audio = pb_.audio_player.initialize(48000, 2);
                }
            }
        }
        lunar::diagnosticLog("rho", "init %dx%d nvdec=%s deko3d=%s audio=%s",
            pb_.frame_w, pb_.frame_h,
            pb_.use_nvdec ? "yes" : "no",
            pb_.use_deko3d ? "yes" : "no",
            have_audio ? "yes" : "no");

        pb_.running = true;
        pb_.worker = std::thread([this]() {
            lunar::diagnosticLog("rho", "worker started nvdec=%s", pb_.use_nvdec ? "true" : "false");
            workerLoop();
            lunar::diagnosticLog("rho", "worker ended");
        });
    }

    void workerLoop() {
        auto last_log = std::chrono::steady_clock::now();
        uint64_t decoded_frames = 0;
        uint64_t video_packets = 0;
        uint64_t video_work_us = 0;
        const double target_fps = pb_.demuxer.videoFps();
        const long long frame_us = target_fps > 0.0
            ? static_cast<long long>((1000000.0 / target_fps) + 0.5)
            : 33333LL;
        const auto frame_interval = std::chrono::microseconds(frame_us);
        auto next_frame_time = std::chrono::steady_clock::now();
        lunar::diagnosticLog("rho", "video target_fps=%.2f frame_interval_us=%lld",
            target_fps, frame_us);

        int iter = 0;
        while (pb_.running) {
            if (iter < 3) lunar::diagnosticLog("rho", "iter=%d", iter);
            iter++;
            AVPacket *vpkt = nullptr, *apkt = nullptr;
            if (!pb_.demuxer.readNext(&vpkt, &apkt)) {
                if (pb_.demuxer.eof) {
                    drainAll();
                    if (pb_.use_nvdec) pb_.nvdec_dec.shutdown();
                    else pb_.video_dec.shutdown();
                    pb_.audio_dec.shutdown();
                    pb_.demuxer.seekToStart();
                    if (pb_.use_nvdec) pb_.nvdec_dec.init(pb_.demuxer.videoPar());
                    else pb_.video_dec.init(pb_.demuxer.videoPar());
                    if (auto* ap = pb_.demuxer.audioPar()) pb_.audio_dec.init(ap);
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (vpkt) {
                auto work_start = std::chrono::steady_clock::now();
                int frames = processVideo(vpkt);
                auto work_end = std::chrono::steady_clock::now();
                video_work_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                    work_end - work_start).count();
                decoded_frames += (uint64_t)frames;
                video_packets++;
                av_packet_free(&vpkt);
                next_frame_time += frame_interval;
                auto sleep_now = std::chrono::steady_clock::now();
                if (sleep_now < next_frame_time) {
                    std::this_thread::sleep_until(next_frame_time);
                } else if (sleep_now - next_frame_time > frame_interval) {
                    next_frame_time = sleep_now;
                }
            }
            if (apkt) {
                processAudio(apkt); av_packet_free(&apkt);
                continue;
            }

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - last_log).count();
            if (elapsed >= 2.0) {
                double fps = elapsed > 0.0 ? (double)decoded_frames / elapsed : 0.0;
                double avg_ms = decoded_frames > 0
                    ? (double)video_work_us / 1000.0 / (double)decoded_frames
                    : 0.0;
                lunar::diagnosticLog("rho",
                    "FPS=%.1f frames=%llu packets=%llu target=%.1f avg_video_ms=%.2f",
                    fps,
                    (unsigned long long)decoded_frames,
                    (unsigned long long)video_packets,
                    target_fps,
                    avg_ms);
                decoded_frames = 0;
                video_packets = 0;
                video_work_us = 0;
                last_log = now;
            }
        }
    }

    int processVideo(AVPacket* pkt) {
        std::vector<AVFrame*> frames;
        if (pb_.use_nvdec) {
            if (!pb_.nvdec_dec.decode(pkt, frames)) return 0;
        } else {
            if (!pb_.video_dec.decode(pkt, frames)) return 0;
        }
        int rendered = 0;
        for (auto* f : frames) {
            if (pb_.use_deko3d) {
                if (pb_.deko3d.uploadNV12(const_cast<const uint8_t**>(f->data), f->linesize, f->width, f->height)) {
                    rendered++;
                }
                av_frame_free(&f);
                continue;
            }
            // nanovg path: sws_scale to RGBA
            auto& sws = pb_.video_sws;
            sws = sws_getCachedContext(sws,
                f->width, f->height, static_cast<AVPixelFormat>(f->format),
                f->width, f->height, AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) { av_frame_free(&f); continue; }

            uint8_t* dst[1] = { pb_.rgba_back.data() };
            int ds[1] = { pb_.frame_w * 4 };
            sws_scale(sws, f->data, f->linesize, 0, pb_.frame_h, dst, ds);
            {
                std::lock_guard<std::mutex> lk(pb_.frame_mutex);
                pb_.rgba_front.swap(pb_.rgba_back);
                pb_.has_frame = true;
                if (pb_.nvg_image < 0) pb_.nvg_image = nvgCreateImageRGBA(
                    brls::Application::getNVGContext(), pb_.frame_w, pb_.frame_h, 0, pb_.rgba_front.data());
            }
            rendered++;
            av_frame_free(&f);
        }
        return rendered;
    }

    void processAudio(AVPacket* pkt) {
        if (!pb_.audio_swr) { lunar::diagnosticLog("rho", "audio: no swr"); return; }
        std::vector<AVFrame*> frames;
        if (!pb_.audio_dec.decode(pkt, frames)) {
            lunar::diagnosticLog("rho", "audio: decode failed");
            return;
        }
        for (auto* f : frames) {
            int dst_nb = av_rescale_rnd(swr_get_delay(pb_.audio_swr, 48000) + f->nb_samples,
                                        48000, f->sample_rate, AV_ROUND_UP);
            std::vector<uint8_t> buf(dst_nb * 2 * 2);
            uint8_t* ptr = buf.data();
            int out = swr_convert(pb_.audio_swr, &ptr, dst_nb,
                                  (const uint8_t**)f->data, f->nb_samples);
            if (out > 0) {
                lunar::stream::AudioFrame af;
                af.sample_rate = 48000; af.channels = 2;
                af.sample_count = (size_t)out;
                af.pcm_data.assign(buf.data(), buf.data() + out * 2 * 2);
                pb_.audio_player.play(af);
            }
            av_frame_free(&f);
        }
    }

    void drainAll() {
        std::vector<AVFrame*> frames;
        if (pb_.use_nvdec) pb_.nvdec_dec.drain(frames);
        else pb_.video_dec.drain(frames);
        for (auto* f : frames) {
            if (pb_.use_deko3d) {
                pb_.deko3d.uploadNV12(const_cast<const uint8_t**>(f->data), f->linesize, f->width, f->height);
                av_frame_free(&f);
                continue;
            }
            auto& sws = pb_.video_sws;
            sws = sws_getCachedContext(sws,
                f->width, f->height, static_cast<AVPixelFormat>(f->format),
                f->width, f->height, AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) { av_frame_free(&f); continue; }
            uint8_t* dst[1] = { pb_.rgba_back.data() };
            int ds[1] = { pb_.frame_w * 4 };
            sws_scale(sws, f->data, f->linesize, 0, pb_.frame_h, dst, ds);
            {
                std::lock_guard<std::mutex> lk(pb_.frame_mutex);
                pb_.rgba_front.swap(pb_.rgba_back);
                pb_.has_frame = true;
                if (pb_.nvg_image < 0) pb_.nvg_image = nvgCreateImageRGBA(
                    brls::Application::getNVGContext(), pb_.frame_w, pb_.frame_h, 0, pb_.rgba_front.data());
            }
            av_frame_free(&f);
        }
        frames.clear();
        pb_.audio_dec.drain(frames);
        for (auto* f : frames) {
            if (!pb_.audio_swr) { av_frame_free(&f); continue; }
            int dst_nb = av_rescale_rnd(swr_get_delay(pb_.audio_swr, 48000) + f->nb_samples,
                                        48000, f->sample_rate, AV_ROUND_UP);
            std::vector<uint8_t> buf(dst_nb * 2 * 2);
            uint8_t* ptr = buf.data();
            int out = swr_convert(pb_.audio_swr, &ptr, dst_nb,
                                  (const uint8_t**)f->data, f->nb_samples);
            if (out > 0) {
                lunar::stream::AudioFrame af;
                af.sample_rate = 48000; af.channels = 2; af.sample_count = out;
                af.pcm_data.assign(buf.data(), buf.data() + out * 2 * 2);
                pb_.audio_player.play(af);
            }
            av_frame_free(&f);
        }
    }

    void stopPlayback() {
        pb_.running = false;
        if (pb_.worker.joinable()) pb_.worker.join();
        pb_.video_dec.shutdown(); pb_.nvdec_dec.shutdown(); pb_.audio_dec.shutdown();
        pb_.deko3d.shutdown();
        pb_.audio_player.shutdown();
        pb_.demuxer.close();
        if (pb_.video_sws) { sws_freeContext(pb_.video_sws); pb_.video_sws = nullptr; }
        if (pb_.audio_swr) { swr_free(&pb_.audio_swr); pb_.audio_swr = nullptr; }
        if (pb_.nvg_image >= 0) {
            nvgDeleteImage(brls::Application::getNVGContext(), pb_.nvg_image);
        }
    }
};

int runSwitch(const char* path) {
    lunar::diagnosticLog("rho", "borealis init nvdec=%s", kRhoUseNvdec ? "true" : "false");
    if (!brls::Application::init()) return 1;
    brls::Application::createWindow("rho");
    brls::Application::setGlobalQuit(false);
    brls::Application::setFPSStatus(false);
    brls::Application::pushActivity(new RhoActivity(path));
    while (brls::Application::mainLoop()) {}
    return 0;
}
#endif

} // namespace
} // namespace lunar::rho

int main(int argc, char* argv[]) {
    const char* path = (argc > 1) ? argv[1] : lunar::rho::defaultVideoPath();
#ifdef __SWITCH__
    return lunar::rho::runSwitch(path);
#else
    return 1;
#endif
}
