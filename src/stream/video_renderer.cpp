#include "video_renderer.h"
#include "../diagnostics.h"
#include "perf_stats.h"
#include "software_video_frame.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <optional>
#include <utility>
#include <vector>
#include <array>
#ifdef __SWITCH__
#include <deko3d.hpp>
#include <borealis.hpp>
#include <borealis/platforms/switch/switch_video.hpp>
#include <nanovg/framework/CMemPool.h>
#include <nanovg/framework/CShader.h>
#include <nanovg/framework/CCmdMemRing.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_nvtegra.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
namespace lunar::stream {
namespace {
  static constexpr unsigned UpdateCmdSliceSize = 0x1000;
  static constexpr unsigned PresentCmdSliceSize = 0x8000;
  static constexpr unsigned DirectCmdSize = 0x10000;
  static constexpr int RenderLogLimit = 64;

  using RenderClock = std::chrono::steady_clock;
  std::atomic<int> render_logs{0};

  bool shouldLogRender() {
    return render_logs.fetch_add(1) < RenderLogLimit;
  }

  struct Vertex { float position[3]; float uv[2]; };

  constexpr std::array<DkVtxAttribState, 2> kVertexAttribState = {{
    DkVtxAttribState{0, 0, offsetof(Vertex, position), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    DkVtxAttribState{0, 0, offsetof(Vertex, uv),       DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
  }};

  constexpr std::array<DkVtxBufferState, 1> kVertexBufferState = {{
    DkVtxBufferState{sizeof(Vertex), 0},
  }};

  constexpr std::array<Vertex, 4> kQuadVertices = {{
    {{-1.0f, +1.0f, 0.0f}, {0.0f, 0.0f}},
    {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
    {{+1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
    {{+1.0f, +1.0f, 0.0f}, {1.0f, 0.0f}},
  }};

  static constexpr float kDefaultRcasStrength = 0.2f;
  static constexpr float kDefaultDitheringStrength = 3.0f;

  struct Tf{ alignas(16) float c0[4],c1[4],c2[4]; alignas(16) float o[4]; alignas(16) float u[4]; };

  struct EasuConstants {
    alignas(16) uint32_t con0[4];
    alignas(16) uint32_t con1[4];
    alignas(16) uint32_t con2[4];
    alignas(16) uint32_t con3[4];
  };

  struct RcasConstants {
    alignas(16) uint32_t control[4];
  };

  struct DitheringConstants {
    alignas(16) float control[4];
  };

  static_assert(sizeof(EasuConstants) == 64, "EasuConstants must match std140 layout");
  static_assert(sizeof(RcasConstants) == 16, "RcasConstants must match std140 layout");
  static_assert(sizeof(DitheringConstants) == 16, "DitheringConstants must match std140 layout");

  uint64_t toMicroseconds(RenderClock::duration duration) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
  }

  struct SourceViewport {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
  };

  uint32_t floatToBits(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
  }

  SourceViewport getSourceViewport(int frame_width, int frame_height,
                                   int screen_width, int screen_height) {
    SourceViewport viewport{
      0.0f,
      0.0f,
      static_cast<float>(frame_width),
      static_cast<float>(frame_height),
    };

    if (frame_width <= 0 || frame_height <= 0 ||
        screen_width <= 0 || screen_height <= 0) {
      return viewport;
    }

    const float frame_aspect = static_cast<float>(frame_height) /
                               static_cast<float>(frame_width);
    const float screen_aspect = static_cast<float>(screen_height) /
                                static_cast<float>(screen_width);

    if (frame_aspect > screen_aspect) {
      const float multiplier = frame_aspect / screen_aspect;
      viewport.width = static_cast<float>(frame_width) / multiplier;
      viewport.offset_x = 0.5f * (static_cast<float>(frame_width) - viewport.width);
    } else {
      const float multiplier = screen_aspect / frame_aspect;
      viewport.height = static_cast<float>(frame_height) / multiplier;
      viewport.offset_y = 0.5f * (static_cast<float>(frame_height) - viewport.height);
    }

    return viewport;
  }

  struct ColorTransform {
    float c0[3];
    float c1[3];
    float c2[3];
    float offset[3];
  };

  ColorTransform colorTransformSpec(AVColorSpace color_space, bool color_full) {
    static constexpr ColorTransform bt601_limited{
        {1.1644f, 1.1644f, 1.1644f},
        {0.0f, -0.3917f, 2.0172f},
        {1.5960f, -0.8129f, 0.0f},
        {16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f},
    };
    static constexpr ColorTransform bt601_full{
        {1.0f, 1.0f, 1.0f},
        {0.0f, -0.3441f, 1.7720f},
        {1.4020f, -0.7141f, 0.0f},
        {0.0f, 128.0f / 255.0f, 128.0f / 255.0f},
    };
    static constexpr ColorTransform bt709_limited{
        {1.1644f, 1.1644f, 1.1644f},
        {0.0f, -0.2132f, 2.1124f},
        {1.7927f, -0.5329f, 0.0f},
        {16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f},
    };
    static constexpr ColorTransform bt709_full{
        {1.0f, 1.0f, 1.0f},
        {0.0f, -0.1873f, 1.8556f},
        {1.5748f, -0.4681f, 0.0f},
        {0.0f, 128.0f / 255.0f, 128.0f / 255.0f},
    };
    static constexpr ColorTransform bt2020_limited{
        {1.1644f, 1.1644f, 1.1644f},
        {0.0f, -0.1874f, 2.1418f},
        {1.6781f, -0.6505f, 0.0f},
        {16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f},
    };
    static constexpr ColorTransform bt2020_full{
        {1.0f, 1.0f, 1.0f},
        {0.0f, -0.1646f, 1.8814f},
        {1.4746f, -0.5714f, 0.0f},
        {0.0f, 128.0f / 255.0f, 128.0f / 255.0f},
    };

    switch (color_space) {
      case AVCOL_SPC_BT709:
        return color_full ? bt709_full : bt709_limited;
      case AVCOL_SPC_BT2020_NCL:
      case AVCOL_SPC_BT2020_CL:
        return color_full ? bt2020_full : bt2020_limited;
      case AVCOL_SPC_SMPTE170M:
      case AVCOL_SPC_BT470BG:
      default:
        return color_full ? bt601_full : bt601_limited;
    }
  }

  void getFrameColorInfo(AVFrame* frame, AVColorSpace& color_space, bool& color_full) {
    color_full = frame && frame->color_range == AVCOL_RANGE_JPEG;
    if (frame && frame->format == AV_PIX_FMT_NVTEGRA &&
        frame->color_range == AVCOL_RANGE_JPEG) {
      color_full = false;
    }

    switch (frame ? frame->colorspace : AVCOL_SPC_UNSPECIFIED) {
      case AVCOL_SPC_SMPTE170M:
      case AVCOL_SPC_BT470BG:
        color_space = AVCOL_SPC_SMPTE170M;
        break;
      case AVCOL_SPC_BT709:
        color_space = AVCOL_SPC_BT709;
        break;
      case AVCOL_SPC_BT2020_NCL:
      case AVCOL_SPC_BT2020_CL:
        color_space = AVCOL_SPC_BT2020_NCL;
        break;
      default:
        color_space = AVCOL_SPC_SMPTE170M;
        break;
    }
  }

  Tf makeTransform(const SourceViewport& viewport,
                   int frame_width,
                   int frame_height,
                   AVColorSpace color_space,
                   bool color_full) {
    const ColorTransform spec = colorTransformSpec(color_space, color_full);
    Tf transform{{spec.c0[0], spec.c0[1], spec.c0[2], 0.0f},
                 {spec.c1[0], spec.c1[1], spec.c1[2], 0.0f},
                 {spec.c2[0], spec.c2[1], spec.c2[2], 0.0f},
                 {spec.offset[0], spec.offset[1], spec.offset[2], 0.0f},
                 {0.0f, 0.0f, 1.0f, 1.0f}};

    if (frame_width > 0 && frame_height > 0 &&
        viewport.width > 0.0f && viewport.height > 0.0f) {
      transform.u[0] = viewport.offset_x / static_cast<float>(frame_width);
      transform.u[1] = viewport.offset_y / static_cast<float>(frame_height);
      transform.u[2] = static_cast<float>(frame_width) / viewport.width;
      transform.u[3] = static_cast<float>(frame_height) / viewport.height;
    }

    return transform;
  }

  Tf fullFrameTransform(int frame_width,
                        int frame_height,
                        AVColorSpace color_space,
                        bool color_full) {
    return makeTransform(SourceViewport{0.0f, 0.0f,
                                        static_cast<float>(frame_width),
                                        static_cast<float>(frame_height)},
                         frame_width, frame_height, color_space, color_full);
  }

  Tf displayTransform(int frame_width,
                      int frame_height,
                      int screen_width,
                      int screen_height,
                      AVColorSpace color_space,
                      bool color_full) {
    return makeTransform(getSourceViewport(frame_width, frame_height, screen_width, screen_height),
                         frame_width, frame_height, color_space, color_full);
  }

  void populateEasuConstants(EasuConstants& constants,
                             const SourceViewport& viewport,
                             int input_width,
                             int input_height,
                             int output_width,
                             int output_height) {
    if (input_width <= 0 || input_height <= 0 ||
        output_width <= 0 || output_height <= 0) {
      constants = {};
      return;
    }

    const float input_width_rcp = 1.0f / static_cast<float>(input_width);
    const float input_height_rcp = 1.0f / static_cast<float>(input_height);
    const float output_width_rcp = 1.0f / static_cast<float>(output_width);
    const float output_height_rcp = 1.0f / static_cast<float>(output_height);

    constants.con0[0] = floatToBits(viewport.width * output_width_rcp);
    constants.con0[1] = floatToBits(viewport.height * output_height_rcp);
    constants.con0[2] = floatToBits(
        0.5f * viewport.width * output_width_rcp - 0.5f + viewport.offset_x);
    constants.con0[3] = floatToBits(
        0.5f * viewport.height * output_height_rcp - 0.5f + viewport.offset_y);

    constants.con1[0] = floatToBits(input_width_rcp);
    constants.con1[1] = floatToBits(input_height_rcp);
    constants.con1[2] = floatToBits(1.0f * input_width_rcp);
    constants.con1[3] = floatToBits(-1.0f * input_height_rcp);

    constants.con2[0] = floatToBits(-1.0f * input_width_rcp);
    constants.con2[1] = floatToBits(2.0f * input_height_rcp);
    constants.con2[2] = floatToBits(1.0f * input_width_rcp);
    constants.con2[3] = floatToBits(2.0f * input_height_rcp);

    constants.con3[0] = 0;
    constants.con3[1] = floatToBits(4.0f * input_height_rcp);
    constants.con3[2] = 0;
    constants.con3[3] = 0;
  }

  void populateRcasConstants(RcasConstants& constants, float strength) {
    const float sharpness_stops = 2.0f * (1.0f - strength);
    const float sharpness_linear = std::exp2(-sharpness_stops);
    constants.control[0] = floatToBits(sharpness_linear);
    constants.control[1] = 0;
    constants.control[2] = 0;
    constants.control[3] = 0;
  }

  bool usesUpscale(PostProcessMode mode) {
    return mode == PostProcessMode::Upscale ||
           mode == PostProcessMode::UpscaleRcas;
  }

  bool usesRcas(PostProcessMode mode) {
    return mode == PostProcessMode::UpscaleRcas;
  }

  enum class RenderPassKind {
    Nv12ToFramebuffer,
    Nv12ToSourceTarget,
    PostProcessBlit,
    Upscale,
    Rcas,
  };

  struct RenderPassConfig {
    RenderPassKind kind = RenderPassKind::Nv12ToFramebuffer;
    bool enabled = false;
  };

  struct RenderPipelineConfig {
    std::array<RenderPassConfig, 5> passes{};
    size_t pass_count = 0;
    PostProcessMode mode = PostProcessMode::Off;
    bool post_process_enabled = false;
    bool upscaling_enabled = false;
    bool rcas_enabled = false;
  };

  struct FrameMapping {
    uint32_t h=0;
    void* a=nullptr;
    uint32_t sz=0,co=0;
    int w=0,hgt=0;
    dk::UniqueMemBlock mb{};
    dk::Image lu{},ch{};
    dk::ImageDescriptor ld{},cd{};

    bool matches(uint32_t handle, void* addr, uint32_t size,
                 uint32_t chroma_offset, int width, int height) const {
      return h == handle && a == addr && sz == size && co == chroma_offset &&
             w == width && hgt == height;
    }
  };

  struct RenderTarget {
    CMemPool::Handle handle;
    dk::ImageLayout layout;
    dk::Image image;
    dk::ImageDescriptor descriptor;
    int texture_slot = -1;
    int width = 0;
    int height = 0;
    bool descriptor_current = false;

    bool allocated() const {
      return static_cast<bool>(handle);
    }
  };

  bool canUpscaleFrame(int frame_width, int frame_height,
                       int target_width, int target_height) {
    return frame_width > 0 && frame_height > 0 &&
           target_width > 0 && target_height > 0 &&
           (target_width > frame_width || target_height > frame_height);
  }

  bool canPostProcessFrame(int frame_width, int frame_height,
                           int target_width, int target_height) {
    return frame_width > 0 && frame_height > 0 &&
           target_width > 0 && target_height > 0;
  }

  void configurePresentPipeline(RenderPipelineConfig& pipeline,
                                PostProcessMode mode,
                                bool post_process_enabled,
                                bool upscaling_enabled,
                                bool rcas_enabled) {
    pipeline = {};
    pipeline.mode = mode;
    pipeline.post_process_enabled = post_process_enabled;
    pipeline.upscaling_enabled = upscaling_enabled;
    pipeline.rcas_enabled = rcas_enabled;
    if (pipeline.upscaling_enabled) {
      pipeline.passes[pipeline.pass_count++] = {RenderPassKind::Nv12ToSourceTarget, true};
      pipeline.passes[pipeline.pass_count++] = {RenderPassKind::Upscale, true};
      if (pipeline.rcas_enabled) {
        pipeline.passes[pipeline.pass_count++] = {RenderPassKind::Rcas, true};
      }
      pipeline.passes[pipeline.pass_count++] = {RenderPassKind::PostProcessBlit, true};
    } else if (pipeline.rcas_enabled) {
      pipeline.passes[pipeline.pass_count++] = {RenderPassKind::Nv12ToSourceTarget, true};
      pipeline.passes[pipeline.pass_count++] = {RenderPassKind::Rcas, true};
      pipeline.passes[pipeline.pass_count++] = {RenderPassKind::PostProcessBlit, true};
    } else {
      pipeline.passes[pipeline.pass_count++] = {RenderPassKind::Nv12ToFramebuffer, true};
    }
  }

  void configurePresentPipeline(RenderPipelineConfig& pipeline,
                                PostProcessMode mode) {
    configurePresentPipeline(pipeline, mode, usesUpscale(mode) || usesRcas(mode),
                             usesUpscale(mode), usesRcas(mode));
  }
}

struct Deko3DRenderContext {
  bool ok=false;
  dk::Device dev=nullptr;
  dk::Queue q=nullptr;
  brls::SwitchVideoContext* vctx=nullptr;
  std::optional<CMemPool> pc,pd,pi;
  dk::UniqueCmdBuf update_cb{};
  dk::UniqueCmdBuf present_cb{};
  dk::UniqueCmdBuf direct_cb{};
  DkCmdList cl=0;
  DkCmdList direct_cl=0;
  CShader vs,fs,blit_fs,upscaling_fs,rcas_fs;
  CMemPool::Handle tu;
  CMemPool::Handle easu_uniform;
  CMemPool::Handle rcas_uniform;
  CMemPool::Handle dithering_uniform;
  CMemPool::Handle vertex_buf;
  CMemPool::Handle direct_cmd_mem;
  std::vector<FrameMapping> fms;
  int fi=-1;
  int li=-1,ci=-1;
  dk::ImageLayout ll,cll;
  std::optional<CCmdMemRing<brls::FRAMEBUFFERS_COUNT>> update_ring;
  std::optional<CCmdMemRing<brls::FRAMEBUFFERS_COUNT>> present_ring;
  RenderTarget source_target;
  RenderTarget upscale_target;
  RenderTarget rcas_target;
  int frame_w=1280, frame_h=720;
  int target_w=1280, target_h=720;
  AVColorSpace color_space=AVCOL_SPC_SMPTE170M;
  bool color_full=false;
  RenderPipelineConfig pipeline;
  PostProcessMode post_process_mode_requested=PostProcessMode::Off;
  float rcas_strength=kDefaultRcasStrength;
  bool dithering_enabled=false;
  float dithering_strength=kDefaultDitheringStrength;
  bool static_state_dirty=true;
  std::mutex render_mutex;
  AVFrame* pending_frame=nullptr;
  AVFrame* current_frame=nullptr;
  std::array<AVFrame*, brls::FRAMEBUFFERS_COUNT + 1> retired_frames{};
  size_t next_retired_frame=0;
};

namespace {

void releaseRetainedFrames(Deko3DRenderContext& s) {
  if(s.pending_frame)av_frame_free(&s.pending_frame);
  if(s.current_frame)av_frame_free(&s.current_frame);
  for(auto*& frame:s.retired_frames){
    if(frame)av_frame_free(&frame);
  }
  s.next_retired_frame=0;
}

void resetRenderTarget(RenderTarget& target) {
  target.handle.destroy();
  target.layout = dk::ImageLayout{};
  target.image = dk::Image{};
  target.descriptor = dk::ImageDescriptor{};
  target.width = 0;
  target.height = 0;
  target.descriptor_current = false;
}

bool allocateRenderTarget(Deko3DRenderContext& s, RenderTarget& target,
                          int width, int height) {
  if (!s.pi || target.texture_slot < 0 || width <= 0 || height <= 0) return false;
  if (target.allocated() && target.width == width && target.height == height) {
    return true;
  }

  resetRenderTarget(target);
  dk::ImageLayoutMaker{s.dev}
      .setType(DkImageType_2D)
      .setFormat(DkImageFormat_RGBA8_Unorm)
      .setDimensions(width, height, 1)
      .setFlags(DkImageFlags_UsageRender |
                DkImageFlags_UsageLoadStore |
                DkImageFlags_Usage2DEngine)
      .initialize(target.layout);

  target.handle = s.pi->allocate(target.layout.getSize(),
                                 target.layout.getAlignment());
  if (!target.handle) return false;

  target.image.initialize(target.layout, target.handle.getMemBlock(),
                          target.handle.getOffset());
  target.descriptor.initialize(target.image);
  target.width = width;
  target.height = height;
  target.descriptor_current = false;
  return true;
}

bool updateRenderTargetDescriptor(Deko3DRenderContext& s, RenderTarget& target) {
  if (!target.allocated() || target.texture_slot < 0) return false;
  s.update_ring->begin(s.update_cb);
  bool updated = s.vctx->updateImageDescriptor(s.update_cb,
                                               target.texture_slot,
                                               target.descriptor);
  if (updated) s.vctx->invalidateImageDescriptors(s.update_cb);
  s.q.submitCommands(s.update_ring->end(s.update_cb));
  target.descriptor_current = updated;
  return updated;
}

void releasePostProcessTargets(Deko3DRenderContext& s);

bool ensurePostProcessTarget(Deko3DRenderContext& s, RenderTarget& target,
                             int width, int height, const char* name) {
  if (!allocateRenderTarget(s, target, width, height)) {
    fprintf(stderr, "[render] %s render target alloc fail\n", name);
    return false;
  }
  if (!target.descriptor_current &&
      !updateRenderTargetDescriptor(s, target)) {
    fprintf(stderr, "[render] %s render target descriptor update fail\n", name);
    resetRenderTarget(target);
    return false;
  }
  return true;
}

bool ensurePostProcessTargets(Deko3DRenderContext& s, bool need_upscale) {
  if (!s.blit_fs) {
    fprintf(stderr, "[render] post-process resources unavailable, direct path only\n");
    releasePostProcessTargets(s);
    return false;
  }

  if (need_upscale && (!s.upscaling_fs || !s.easu_uniform)) {
    fprintf(stderr, "[render] EASU post-process resources unavailable, direct path only\n");
    releasePostProcessTargets(s);
    return false;
  }

  if (need_upscale) {
    if (!ensurePostProcessTarget(s, s.source_target, s.frame_w, s.frame_h, "source")) {
      releasePostProcessTargets(s);
      return false;
    }
  } else {
    resetRenderTarget(s.source_target);
  }

  if (!ensurePostProcessTarget(s, s.upscale_target, s.target_w, s.target_h, "post-process")) {
    releasePostProcessTargets(s);
    return false;
  }

  return true;
}

bool ensureRcasTarget(Deko3DRenderContext& s) {
  if (!s.rcas_fs || !s.rcas_uniform) {
    fprintf(stderr, "[render] RCAS resources unavailable, falling back to base post pass\n");
    resetRenderTarget(s.rcas_target);
    return false;
  }
  if (!ensurePostProcessTarget(s, s.rcas_target, s.target_w, s.target_h, "rcas")) {
    fprintf(stderr, "[render] RCAS target unavailable, falling back to base post pass\n");
    resetRenderTarget(s.rcas_target);
    return false;
  }
  return true;
}

void releasePostProcessTargets(Deko3DRenderContext& s) {
  resetRenderTarget(s.source_target);
  resetRenderTarget(s.upscale_target);
  resetRenderTarget(s.rcas_target);
}

void refreshTargetSize(Deko3DRenderContext& s) {
  int width = brls::Application::windowWidth > 0
      ? static_cast<int>(brls::Application::windowWidth)
      : static_cast<int>(brls::Application::ORIGINAL_WINDOW_WIDTH);
  int height = brls::Application::windowHeight > 0
      ? static_cast<int>(brls::Application::windowHeight)
      : static_cast<int>(brls::Application::ORIGINAL_WINDOW_HEIGHT);
  if (width != s.target_w || height != s.target_h) {
    s.target_w = width;
    s.target_h = height;
    s.static_state_dirty = true;
  }
}

void recordFramebufferState(Deko3DRenderContext& s, dk::Image* fb, dk::Image* db) {
  dk::ImageView colorTarget{*fb};
  dk::ImageView depthTarget{*db};
  s.present_cb.bindRenderTargets(&colorTarget,&depthTarget);
  s.present_cb.setViewports(0,{{{0.0f,0.0f,static_cast<float>(s.target_w),static_cast<float>(s.target_h),0.0f,1.0f}}});
  s.present_cb.setScissors(0,{{{0,0,static_cast<uint32_t>(s.target_w),static_cast<uint32_t>(s.target_h)}}});

  dk::RasterizerState rasterizerState;
  dk::DepthStencilState depthStencilState;
  dk::ColorState colorState;
  dk::ColorWriteState colorWriteState;
  s.present_cb.bindRasterizerState(rasterizerState);
  s.present_cb.bindDepthStencilState(depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilTestEnable(false));
  s.present_cb.bindColorState(colorState);
  s.present_cb.bindColorWriteState(colorWriteState);

  s.present_cb.bindVtxBuffer(0,s.vertex_buf.getGpuAddr(),s.vertex_buf.getSize());
  s.present_cb.bindVtxAttribState(kVertexAttribState);
  s.present_cb.bindVtxBufferState(kVertexBufferState);
}

void recordRenderTargetState(Deko3DRenderContext& s, RenderTarget& target) {
  dk::ImageView colorTarget{target.image};
  s.present_cb.bindRenderTargets(&colorTarget);
  s.present_cb.setViewports(0,{{{0.0f,0.0f,static_cast<float>(target.width),static_cast<float>(target.height),0.0f,1.0f}}});
  s.present_cb.setScissors(0,{{{0,0,static_cast<uint32_t>(target.width),static_cast<uint32_t>(target.height)}}});

  dk::RasterizerState rasterizerState;
  dk::DepthStencilState depthStencilState;
  dk::ColorState colorState;
  dk::ColorWriteState colorWriteState;
  s.present_cb.bindRasterizerState(rasterizerState);
  s.present_cb.bindDepthStencilState(depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilTestEnable(false));
  s.present_cb.bindColorState(colorState);
  s.present_cb.bindColorWriteState(colorWriteState);

  s.present_cb.bindVtxBuffer(0,s.vertex_buf.getGpuAddr(),s.vertex_buf.getSize());
  s.present_cb.bindVtxAttribState(kVertexAttribState);
  s.present_cb.bindVtxBufferState(kVertexBufferState);
}

void recordNv12ToFramebufferPass(Deko3DRenderContext& s, const Tf& transform) {
  s.present_cb.bindShaders(DkStageFlag_GraphicsMask,{s.vs,s.fs});
  s.present_cb.bindTextures(DkStage_Fragment,0,dkMakeTextureHandle(s.li,0));
  s.present_cb.bindTextures(DkStage_Fragment,1,dkMakeTextureHandle(s.ci,0));
  s.present_cb.bindUniformBuffer(DkStage_Fragment,0,s.tu.getGpuAddr(),s.tu.getSize());
  s.present_cb.pushConstants(s.tu.getGpuAddr(),sizeof(Tf),0,sizeof(Tf),&transform);
  s.present_cb.draw(DkPrimitive_Quads,kQuadVertices.size(),1,0,0);
}

void updateDitheringUniform(Deko3DRenderContext& s, bool enabled) {
  if (!s.dithering_uniform) return;
  DitheringConstants constants{};
  constants.control[0] = enabled ? 1.0f : 0.0f;
  constants.control[1] = std::clamp(s.dithering_strength, 1.0f, 10.0f);
  memcpy(s.dithering_uniform.getCpuAddr(), &constants, sizeof(constants));
}

void recordPostProcessBlitPass(Deko3DRenderContext& s,
                               int texture_slot,
                               bool enable_dithering) {
  updateDitheringUniform(s, enable_dithering);
  s.present_cb.bindShaders(DkStageFlag_GraphicsMask,{s.vs,s.blit_fs});
  s.present_cb.bindTextures(DkStage_Fragment,0,dkMakeTextureHandle(texture_slot,0));
  if (s.dithering_uniform) {
    s.present_cb.bindUniformBuffer(DkStage_Fragment,0,
                                   s.dithering_uniform.getGpuAddr(),
                                   s.dithering_uniform.getSize());
  }
  s.present_cb.draw(DkPrimitive_Quads,kQuadVertices.size(),1,0,0);
}

void updateEasuUniform(Deko3DRenderContext& s) {
  EasuConstants constants = {};
  populateEasuConstants(constants,
                        getSourceViewport(s.frame_w, s.frame_h, s.target_w, s.target_h),
                        s.frame_w,
                        s.frame_h,
                        s.target_w,
                        s.target_h);
  memcpy(s.easu_uniform.getCpuAddr(), &constants, sizeof(constants));
}

void updateRcasUniform(Deko3DRenderContext& s) {
  RcasConstants constants = {};
  populateRcasConstants(constants, s.rcas_strength);
  memcpy(s.rcas_uniform.getCpuAddr(), &constants, sizeof(constants));
}

void recordUpscalePass(Deko3DRenderContext& s) {
  updateEasuUniform(s);
  s.present_cb.bindShaders(DkStageFlag_GraphicsMask,{s.vs,s.upscaling_fs});
  s.present_cb.bindTextures(DkStage_Fragment,0,dkMakeTextureHandle(s.source_target.texture_slot,0));
  s.present_cb.bindUniformBuffer(DkStage_Fragment,0,s.easu_uniform.getGpuAddr(),s.easu_uniform.getSize());
  s.present_cb.draw(DkPrimitive_Quads,kQuadVertices.size(),1,0,0);
}

void recordRcasPass(Deko3DRenderContext& s) {
  updateRcasUniform(s);
  s.present_cb.bindShaders(DkStageFlag_GraphicsMask,{s.vs,s.rcas_fs});
  s.present_cb.bindTextures(DkStage_Fragment,0,dkMakeTextureHandle(s.upscale_target.texture_slot,0));
  s.present_cb.bindUniformBuffer(DkStage_Fragment,0,s.rcas_uniform.getGpuAddr(),s.rcas_uniform.getSize());
  s.present_cb.draw(DkPrimitive_Quads,kQuadVertices.size(),1,0,0);
}

void recordPresentPipeline(Deko3DRenderContext& s,
                           dk::Image* fb,
                           dk::Image* db,
                           PerfStats* perf) {
  refreshTargetSize(s);
  bool use_upscale = usesUpscale(s.post_process_mode_requested) &&
      canUpscaleFrame(s.frame_w, s.frame_h, s.target_w, s.target_h);
  bool use_rcas = usesRcas(s.post_process_mode_requested) &&
      canPostProcessFrame(s.frame_w, s.frame_h, s.target_w, s.target_h);
  const bool can_dither = s.dithering_enabled && s.blit_fs && s.dithering_uniform;
  bool use_intermediate = use_upscale || use_rcas || can_dither;

  if (use_intermediate) {
    if (!ensurePostProcessTargets(s, use_upscale)) {
      use_upscale = false;
      use_rcas = false;
      use_intermediate = false;
    } else if (use_rcas) {
      use_rcas = ensureRcasTarget(s);
      use_intermediate = use_upscale || use_rcas || can_dither;
      if (!use_intermediate) {
        releasePostProcessTargets(s);
      }
    } else {
      resetRenderTarget(s.rcas_target);
    }
  } else {
    releasePostProcessTargets(s);
  }
  configurePresentPipeline(s.pipeline, s.post_process_mode_requested,
                           use_intermediate,
                           use_upscale, use_rcas);

  if (s.pipeline.upscaling_enabled) {
    const auto post_start = RenderClock::now();
    recordRenderTargetState(s, s.source_target);
    s.present_cb.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
    recordNv12ToFramebufferPass(s, fullFrameTransform(s.frame_w, s.frame_h,
                                                      s.color_space, s.color_full));
    s.present_cb.barrier(DkBarrier_Tiles, DkInvalidateFlags_Image);

    recordRenderTargetState(s, s.upscale_target);
    s.present_cb.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
    const auto upscale_start = RenderClock::now();
    recordUpscalePass(s);
    s.present_cb.barrier(DkBarrier_Tiles, DkInvalidateFlags_Image);
    if (perf) perf->recordUpscaling(toMicroseconds(RenderClock::now() - upscale_start));

    int final_texture_slot = s.upscale_target.texture_slot;
    if (s.pipeline.rcas_enabled && s.rcas_target.allocated()) {
      recordRenderTargetState(s, s.rcas_target);
      s.present_cb.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
      const auto sharpening_start = RenderClock::now();
      recordRcasPass(s);
      s.present_cb.barrier(DkBarrier_Tiles, DkInvalidateFlags_Image);
      if (perf) perf->recordSharpening(toMicroseconds(RenderClock::now() - sharpening_start));
      final_texture_slot = s.rcas_target.texture_slot;
    }

    recordFramebufferState(s, fb, db);
    const auto dithering_start = RenderClock::now();
    recordPostProcessBlitPass(s, final_texture_slot, can_dither);
    if (can_dither && perf) perf->recordDithering(toMicroseconds(RenderClock::now() - dithering_start));
    if (perf) perf->recordPostProcess(toMicroseconds(RenderClock::now() - post_start));
  } else if (use_intermediate) {
    const auto post_start = RenderClock::now();
    recordRenderTargetState(s, s.upscale_target);
    s.present_cb.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
    recordNv12ToFramebufferPass(s, displayTransform(s.frame_w, s.frame_h,
                                                    s.target_w, s.target_h,
                                                    s.color_space, s.color_full));
    s.present_cb.barrier(DkBarrier_Tiles, DkInvalidateFlags_Image);

    int final_texture_slot = s.upscale_target.texture_slot;
    if (s.pipeline.rcas_enabled && s.rcas_target.allocated()) {
      recordRenderTargetState(s, s.rcas_target);
      s.present_cb.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
      const auto sharpening_start = RenderClock::now();
      recordRcasPass(s);
      s.present_cb.barrier(DkBarrier_Tiles, DkInvalidateFlags_Image);
      if (perf) perf->recordSharpening(toMicroseconds(RenderClock::now() - sharpening_start));
      final_texture_slot = s.rcas_target.texture_slot;
    }

    recordFramebufferState(s, fb, db);
    const auto dithering_start = RenderClock::now();
    recordPostProcessBlitPass(s, final_texture_slot, can_dither);
    if (perf) {
      if (can_dither) {
        perf->recordDithering(toMicroseconds(RenderClock::now() - dithering_start));
      }
      perf->recordPostProcess(toMicroseconds(RenderClock::now() - post_start));
    }
  } else {
    recordFramebufferState(s, fb, db);
    recordNv12ToFramebufferPass(s, displayTransform(s.frame_w, s.frame_h,
                                                    s.target_w, s.target_h,
                                                    s.color_space, s.color_full));
  }
  s.static_state_dirty = false;
}

bool recordDirectPresentCommands(Deko3DRenderContext& s) {
  refreshTargetSize(s);
  if(s.direct_cl && s.q)s.q.waitIdle();
  s.direct_cb.clear();
  s.direct_cb.addMemory(s.direct_cmd_mem.getMemBlock(),
                        s.direct_cmd_mem.getOffset(),
                        s.direct_cmd_mem.getSize());

  dk::RasterizerState rasterizer_state;
  dk::DepthStencilState depth_state;
  dk::ColorState color_state;
  dk::ColorWriteState color_write_state;
  s.direct_cb.setViewports(0,{{{0.0f,0.0f,static_cast<float>(s.target_w),static_cast<float>(s.target_h),0.0f,1.0f}}});
  s.direct_cb.setScissors(0,{{{0,0,static_cast<uint32_t>(s.target_w),static_cast<uint32_t>(s.target_h)}}});
  s.direct_cb.bindRasterizerState(rasterizer_state);
  s.direct_cb.bindDepthStencilState(depth_state.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilTestEnable(false));
  s.direct_cb.bindColorState(color_state);
  s.direct_cb.bindColorWriteState(color_write_state);
  s.direct_cb.bindVtxBuffer(0,s.vertex_buf.getGpuAddr(),s.vertex_buf.getSize());
  s.direct_cb.bindVtxAttribState(kVertexAttribState);
  s.direct_cb.bindVtxBufferState(kVertexBufferState);
  s.direct_cb.clearColor(0,DkColorMask_RGBA,0.0f,0.0f,0.0f,1.0f);
  s.direct_cb.bindShaders(DkStageFlag_GraphicsMask,{s.vs,s.fs});
  s.direct_cb.bindTextures(DkStage_Fragment,0,dkMakeTextureHandle(s.li,0));
  s.direct_cb.bindTextures(DkStage_Fragment,1,dkMakeTextureHandle(s.ci,0));
  s.direct_cb.bindUniformBuffer(DkStage_Fragment,0,s.tu.getGpuAddr(),s.tu.getSize());
  const auto transform=displayTransform(s.frame_w,s.frame_h,s.target_w,s.target_h,
                                        s.color_space,s.color_full);
  s.direct_cb.pushConstants(s.tu.getGpuAddr(),sizeof(Tf),0,sizeof(Tf),&transform);
  s.direct_cb.draw(DkPrimitive_Quads,kQuadVertices.size(),1,0,0);
  s.direct_cl=s.direct_cb.finishList();
  s.static_state_dirty=false;
  return s.direct_cl!=0;
}

bool updateFrameMapping(Deko3DRenderContext& s, AVFrame* frame) {
  if(!frame||frame->format!=AV_PIX_FMT_NVTEGRA)return false;
  AVNVTegraMap* nvmap=av_nvtegra_frame_get_fbuf_map(frame);
  if(!nvmap){
    lunar::diagnosticLog("render","present reject missing nvtegra map width=%d height=%d",frame->width,frame->height);
    return false;
  }

  const uint32_t handle=av_nvtegra_map_get_handle(nvmap);
  void* const address=av_nvtegra_map_get_addr(nvmap);
  const uint32_t size=av_nvtegra_map_get_size(nvmap);
  const uint32_t chroma_offset=static_cast<uint32_t>(
      static_cast<uint8_t*>(frame->data[1])-static_cast<uint8_t*>(frame->data[0]));
  if(!handle||!address||size==0||chroma_offset>=size){
    lunar::diagnosticLog("render","present reject invalid nvtegra map handle=%u addr=%p size=%u chroma=%u",handle,address,size,chroma_offset);
    return false;
  }

  if(frame->width!=s.frame_w||frame->height!=s.frame_h){
    s.q.waitIdle();
    s.fms.clear();
    s.fi=-1;
    s.static_state_dirty=true;
  }

  int mapping_index=-1;
  for(size_t i=0;i<s.fms.size();++i){
    if(s.fms[i].matches(handle,address,size,chroma_offset,frame->width,frame->height)){
      mapping_index=static_cast<int>(i);
      break;
    }
  }

  if(mapping_index<0){
    dk::ImageLayoutMaker{s.dev}
        .setType(DkImageType_2D)
        .setFormat(DkImageFormat_R8_Unorm)
        .setDimensions(frame->width,frame->height,1)
        .setFlags(DkImageFlags_UsageLoadStore|DkImageFlags_Usage2DEngine|DkImageFlags_UsageVideo)
        .initialize(s.ll);
    dk::ImageLayoutMaker{s.dev}
        .setType(DkImageType_2D)
        .setFormat(DkImageFormat_RG8_Unorm)
        .setDimensions(frame->width/2,frame->height/2,1)
        .setFlags(DkImageFlags_UsageLoadStore|DkImageFlags_Usage2DEngine|DkImageFlags_UsageVideo)
        .initialize(s.cll);
    if(s.ll.getSize()>size||chroma_offset+s.cll.getSize()>size){
      lunar::diagnosticLog("render","present reject nvtegra layout handle=%u size=%u chroma=%u luma_layout=%llu chroma_layout=%llu",handle,size,chroma_offset,(unsigned long long)s.ll.getSize(),(unsigned long long)s.cll.getSize());
      return false;
    }

    auto external_mem=dk::MemBlockMaker{s.dev,size}
        .setFlags(DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached|DkMemBlockFlags_Image)
        .setStorage(address)
        .create();
    if(!external_mem){
      lunar::diagnosticLog("render","present reject external memblock handle=%u addr=%p size=%u",handle,address,size);
      return false;
    }

    mapping_index=static_cast<int>(s.fms.size());
    s.fms.emplace_back();
    auto& mapping=s.fms.back();
    mapping.h=handle;
    mapping.a=address;
    mapping.sz=size;
    mapping.co=chroma_offset;
    mapping.w=frame->width;
    mapping.hgt=frame->height;
    mapping.mb=dk::UniqueMemBlock{std::move(external_mem)};
    mapping.lu.initialize(s.ll,mapping.mb,0);
    mapping.ch.initialize(s.cll,mapping.mb,chroma_offset);
    mapping.ld.initialize(mapping.lu);
    mapping.cd.initialize(mapping.ch);
    if(shouldLogRender()){
      lunar::diagnosticLog("render","nvtegra mapping added index=%d handle=%u addr=%p size=%u chroma=%u pitches=%d/%d layouts=%llu/%llu",mapping_index,handle,address,size,chroma_offset,frame->linesize[0],frame->linesize[1],(unsigned long long)s.ll.getSize(),(unsigned long long)s.cll.getSize());
    }
  }

  if(mapping_index!=s.fi){
    s.update_ring->begin(s.update_cb);
    auto& mapping=s.fms[mapping_index];
    const bool luma_ok=s.vctx->updateImageDescriptor(s.update_cb,s.li,mapping.ld);
    const bool chroma_ok=s.vctx->updateImageDescriptor(s.update_cb,s.ci,mapping.cd);
    if(luma_ok&&chroma_ok){
      s.vctx->invalidateImageDescriptors(s.update_cb);
      s.fi=mapping_index;
    }else{
      s.q.submitCommands(s.update_ring->end(s.update_cb));
      lunar::diagnosticLog("render","present reject descriptor update fail");
      return false;
    }
    s.q.submitCommands(s.update_ring->end(s.update_cb));
  }

  s.frame_w=frame->width;
  s.frame_h=frame->height;
  AVColorSpace color_space=s.color_space;
  bool color_full=s.color_full;
  getFrameColorInfo(frame,color_space,color_full);
  if(color_space!=s.color_space||color_full!=s.color_full){
    s.color_space=color_space;
    s.color_full=color_full;
    s.static_state_dirty=true;
  }
  return true;
}

} // namespace

VideoRenderer::VideoRenderer() = default;
VideoRenderer::~VideoRenderer(){shutdown();delete static_cast<Deko3DRenderContext*>(ctx_);ctx_=nullptr;}

void VideoRenderer::setVideoBackend(VideoBackend backend){
  video_backend_=backend;
}

bool VideoRenderer::initialize(const char*,int w,int h){
  if(video_backend_==VideoBackend::Software){
    try{
      lunar::diagnosticLog("render","software renderer init begin width=%d height=%d",w,h);
      if(software_sws_){
        auto* sws=reinterpret_cast<SwsContext*>(software_sws_);
        sws_freeContext(sws);
        software_sws_=nullptr;
      }
      software_rgba_.clear();
      lunar::diagnosticLog("render","software renderer sink clear begin");
      SoftwareVideoFrameSink::instance().clear();
      lunar::diagnosticLog("render","software renderer initialized width=%d height=%d",w,h);
      fprintf(stderr,"[render] software RGBA renderer ok\n");
      return true;
    }catch(const std::exception& e){
      lunar::diagnosticLog("render","software renderer init exception: %s",e.what());
      return false;
    }catch(...){
      lunar::diagnosticLog("render","software renderer init unknown exception");
      return false;
    }
  }
  if(!ctx_){
    lunar::diagnosticLog("render","context alloc begin");
    ctx_=new (std::nothrow) Deko3DRenderContext();
    if(!ctx_){
      lunar::diagnosticLog("render","context alloc failed");
      fprintf(stderr,"[render] context alloc failed\n");
      return false;
    }
    lunar::diagnosticLog("render","context alloc done ptr=%p",ctx_);
  }
  auto* s=static_cast<Deko3DRenderContext*>(ctx_);
  if(s->ok)shutdown();

  try {
    lunar::diagnosticLog("render","initialize begin width=%d height=%d",w,h);
    auto* platform=brls::Application::getPlatform();
    if(!platform){
      lunar::diagnosticLog("render","initialize failed missing platform");
      return false;
    }
    s->vctx=static_cast<brls::SwitchVideoContext*>(platform->getVideoContext());
    if(!s->vctx){
      lunar::diagnosticLog("render","initialize failed missing video context");
      return false;
    }
    s->dev=s->vctx->getDeko3dDevice();s->q=s->vctx->getQueue();
    if(!s->dev||!s->q){
      lunar::diagnosticLog("render","initialize failed dev=%s queue=%s",s->dev?"true":"false",s->q?"true":"false");
      return false;
    }
    s->post_process_mode_requested=requested_post_process_mode_;
    s->dithering_enabled=requested_dithering_enabled_;
    s->dithering_strength=std::clamp(requested_dithering_strength_,1.0f,10.0f);
    s->pc.emplace(s->dev,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached|DkMemBlockFlags_Code,128*1024);
    s->pd.emplace(s->dev,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached,1024*1024);
    s->pi.emplace(s->dev,DkMemBlockFlags_GpuCached|DkMemBlockFlags_Image);
    s->update_cb=dk::CmdBufMaker{s->dev}.create();
    s->present_cb=dk::CmdBufMaker{s->dev}.create();
    s->direct_cb=dk::CmdBufMaker{s->dev}.create();
    s->update_ring.emplace();
    s->present_ring.emplace();
    if(!s->update_ring->allocate(*s->pd,UpdateCmdSliceSize)||
       !s->present_ring->allocate(*s->pd,PresentCmdSliceSize)){
      fprintf(stderr,"[render] cmd ring alloc fail\n");return false;
    }
    s->direct_cmd_mem=s->pd->allocate(DirectCmdSize,DK_CMDMEM_ALIGNMENT);
    if(!s->direct_cmd_mem){fprintf(stderr,"[render] direct cmd alloc fail\n");return false;}
    s->fs.load(*s->pc,"romfs:/shaders/texture_fsh.dksh");
    s->vs.load(*s->pc,"romfs:/shaders/basic_vsh.dksh");
    s->blit_fs.load(*s->pc,"romfs:/shaders/upscaling_pass_fsh.dksh");
    s->upscaling_fs.load(*s->pc,"romfs:/shaders/upscaling_fsh.dksh");
    s->rcas_fs.load(*s->pc,"romfs:/shaders/rcas_fsh.dksh");
    if(!s->fs||!s->vs){fprintf(stderr,"[render] shader fail\n");return false;}
    if(!s->blit_fs){fprintf(stderr,"[render] post-process shader unavailable, direct path only\n");}
    if(!s->upscaling_fs){fprintf(stderr,"[render] EASU shader unavailable, direct path only\n");}
    if(!s->rcas_fs){fprintf(stderr,"[render] RCAS shader unavailable, EASU-only fallback\n");}
    s->tu=s->pd->allocate(sizeof(Tf),DK_UNIFORM_BUF_ALIGNMENT);
    s->easu_uniform=s->pd->allocate(sizeof(EasuConstants),DK_UNIFORM_BUF_ALIGNMENT);
    s->rcas_uniform=s->pd->allocate(sizeof(RcasConstants),DK_UNIFORM_BUF_ALIGNMENT);
    s->dithering_uniform=s->pd->allocate(sizeof(DitheringConstants),DK_UNIFORM_BUF_ALIGNMENT);
    if(!s->tu||!s->easu_uniform||!s->rcas_uniform||!s->dithering_uniform){
      fprintf(stderr,"[render] uniform alloc fail\n");return false;
    }
  } catch (const std::exception& e) {
    lunar::diagnosticLog("render","initialize exception: %s",e.what());
    fprintf(stderr,"[render] initialize exception: %s\n",e.what());
    shutdown();
    return false;
  } catch (...) {
    lunar::diagnosticLog("render","initialize unknown exception");
    fprintf(stderr,"[render] initialize unknown exception\n");
    shutdown();
    return false;
  }
  auto t=fullFrameTransform(w,h,s->color_space,s->color_full);memcpy(s->tu.getCpuAddr(),&t,sizeof(t));
  EasuConstants easu_constants{};
  memcpy(s->easu_uniform.getCpuAddr(),&easu_constants,sizeof(easu_constants));
  RcasConstants rcas_constants{};
  populateRcasConstants(rcas_constants,kDefaultRcasStrength);
  memcpy(s->rcas_uniform.getCpuAddr(),&rcas_constants,sizeof(rcas_constants));
  s->rcas_strength=kDefaultRcasStrength;
  DitheringConstants dithering_constants{};
  dithering_constants.control[0]=s->dithering_enabled?1.0f:0.0f;
  dithering_constants.control[1]=s->dithering_strength;
  memcpy(s->dithering_uniform.getCpuAddr(),&dithering_constants,sizeof(dithering_constants));
  s->vertex_buf=s->pd->allocate(sizeof(kQuadVertices),alignof(Vertex));
  if(!s->vertex_buf){fprintf(stderr,"[render] vertex buffer alloc fail\n");return false;}
  memcpy(s->vertex_buf.getCpuAddr(),kQuadVertices.data(),s->vertex_buf.getSize());
  s->li=s->vctx->allocateImageIndex();s->ci=s->vctx->allocateImageIndex();
  s->source_target.texture_slot=s->vctx->allocateImageIndex();
  s->upscale_target.texture_slot=s->vctx->allocateImageIndex();
  s->rcas_target.texture_slot=s->vctx->allocateImageIndex();
  if(s->li<0||s->ci<0||s->source_target.texture_slot<0||
     s->upscale_target.texture_slot<0||s->rcas_target.texture_slot<0){
    fprintf(stderr,"[render] image slot alloc fail\n");return false;
  }
  s->frame_w=w;s->frame_h=h;
  s->target_w = brls::Application::windowWidth > 0
      ? static_cast<int>(brls::Application::windowWidth)
      : static_cast<int>(brls::Application::ORIGINAL_WINDOW_WIDTH);
  s->target_h = brls::Application::windowHeight > 0
      ? static_cast<int>(brls::Application::windowHeight)
      : static_cast<int>(brls::Application::ORIGINAL_WINDOW_HEIGHT);
  configurePresentPipeline(s->pipeline, s->post_process_mode_requested);
  s->static_state_dirty=true;
  s->ok=true;fprintf(stderr,"[render] deko3d ok\n");return true;
}

void VideoRenderer::setPostProcessMode(PostProcessMode mode){
  requested_post_process_mode_=mode;
  auto* s=static_cast<Deko3DRenderContext*>(ctx_);
  if(!s)return;
  std::lock_guard<std::mutex> lock(s->render_mutex);
  s->post_process_mode_requested=mode;
  configurePresentPipeline(s->pipeline, mode);
  if(mode==PostProcessMode::Off)releasePostProcessTargets(*s);
  s->static_state_dirty=true;
}

void VideoRenderer::setPostProcessEnabled(bool enabled){
  setPostProcessMode(enabled?PostProcessMode::Upscale:PostProcessMode::Off);
}

void VideoRenderer::setDitheringEnabled(bool enabled, float strength){
  requested_dithering_enabled_=enabled;
  requested_dithering_strength_=std::clamp(strength,1.0f,10.0f);
  auto* s=static_cast<Deko3DRenderContext*>(ctx_);
  if(!s)return;
  std::lock_guard<std::mutex> lock(s->render_mutex);
  s->dithering_enabled=enabled;
  s->dithering_strength=std::clamp(strength,1.0f,10.0f);
  if(!enabled && !usesUpscale(s->post_process_mode_requested)){
    releasePostProcessTargets(*s);
  }
  s->static_state_dirty=true;
}

bool VideoRenderer::render(const VideoFrame&frame){
  if(video_backend_==VideoBackend::Software){
    AVFrame* f=frame.avframe;
    if(!f){
      if(shouldLogRender())lunar::diagnosticLog("render","software render reject missing AVFrame");
      return false;
    }
    if(f->width<=0||f->height<=0){
      if(shouldLogRender())lunar::diagnosticLog("render","software render reject invalid size width=%d height=%d",f->width,f->height);
      return false;
    }
    std::lock_guard<std::mutex> lock(software_mutex_);
    auto* sws=reinterpret_cast<SwsContext*>(software_sws_);
    sws=sws_getCachedContext(sws,
                             f->width,
                             f->height,
                             static_cast<AVPixelFormat>(f->format),
                             f->width,
                             f->height,
                             AV_PIX_FMT_RGBA,
                             SWS_FAST_BILINEAR,
                             nullptr,
                             nullptr,
                             nullptr);
    software_sws_=sws;
    if(!sws){
      if(shouldLogRender())lunar::diagnosticLog("render","software render reject sws_getCachedContext failed format=%d",f->format);
      return false;
    }
    const size_t rgba_size=static_cast<size_t>(f->width)*static_cast<size_t>(f->height)*4;
    try{
      software_rgba_.resize(rgba_size);
    }catch(...){
      if(shouldLogRender())lunar::diagnosticLog("render","software render reject rgba alloc failed size=%zu",rgba_size);
      return false;
    }
    uint8_t* dst_data[4]={software_rgba_.data(),nullptr,nullptr,nullptr};
    int dst_linesize[4]={f->width*4,0,0,0};
    const auto render_start=RenderClock::now();
    int rows=sws_scale(sws,
                       f->data,
                       f->linesize,
                       0,
                       f->height,
                       dst_data,
                       dst_linesize);
    if(rows<=0){
      if(shouldLogRender())lunar::diagnosticLog("render","software render reject sws_scale rows=%d",rows);
      return false;
    }
    if(perf_)perf_->recordRenderSubmit(toMicroseconds(RenderClock::now()-render_start));
    const bool published=SoftwareVideoFrameSink::instance().publishRgba(
        software_rgba_.data(),f->width,f->height,frame.timestamp);
    if(shouldLogRender())lunar::diagnosticLog("render","software render success width=%d height=%d rows=%d published=%s",f->width,f->height,rows,published?"true":"false");
    return published;
  }

  auto* s=static_cast<Deko3DRenderContext*>(ctx_);
  if(!s||!s->ok){
    if(shouldLogRender())lunar::diagnosticLog("render","render reject context unavailable");
    return false;
  }
  AVFrame* f=frame.avframe;
  if(!f){
    if(shouldLogRender())lunar::diagnosticLog("render","render reject missing AVFrame");
    return false;
  }
  if(f->format!=AV_PIX_FMT_NVTEGRA){
    if(shouldLogRender())lunar::diagnosticLog("render","render reject format=%d expected=%d",f->format,AV_PIX_FMT_NVTEGRA);
    return false;
  }
  std::lock_guard<std::mutex> lock(s->render_mutex);
  AVFrame* keep=av_frame_alloc();
  if(!keep||av_frame_ref(keep,f)<0){
    if(keep)av_frame_free(&keep);
    if(shouldLogRender())lunar::diagnosticLog("render","render reject frame ref failed");
    return false;
  }
  if(s->pending_frame)av_frame_free(&s->pending_frame);
  s->pending_frame=keep;
  if(shouldLogRender())lunar::diagnosticLog("render","render queued width=%d height=%d",f->width,f->height);
  return true;
}

void VideoRenderer::present(){
  if(video_backend_==VideoBackend::Software)return;
  auto* s=static_cast<Deko3DRenderContext*>(ctx_);
  if(!s||!s->ok){
    if(shouldLogRender())lunar::diagnosticLog("render","present reject context unavailable");
    return;
  }
  std::lock_guard<std::mutex> lock(s->render_mutex);
  if(!s->pending_frame&&!s->current_frame){
    return;
  }
  if(s->pending_frame){
    auto& retired=s->retired_frames[
        s->next_retired_frame++%s->retired_frames.size()];
    if(retired)av_frame_free(&retired);
    retired=s->current_frame;
    s->current_frame=s->pending_frame;
    s->pending_frame=nullptr;
  }
  if(!updateFrameMapping(*s,s->current_frame))return;

  dk::Image* fb = s->vctx->getFramebuffer();
  dk::Image* db = s->vctx->getDepthBuffer();
  if(!fb||!db){
    if(shouldLogRender())lunar::diagnosticLog("render","present reject framebuffer=%s depth=%s",fb?"true":"false",db?"true":"false");
    return;
  }

  const auto render_start = RenderClock::now();
  const bool direct_path=s->post_process_mode_requested==PostProcessMode::Off&&
                         !s->dithering_enabled;
  if(direct_path){
    if((s->static_state_dirty||!s->direct_cl)&&!recordDirectPresentCommands(*s)){
      if(shouldLogRender())lunar::diagnosticLog("render","present reject direct command recording failed");
      return;
    }
    s->q.submitCommands(s->direct_cl);
  }else{
    s->present_ring->begin(s->present_cb);
    recordPresentPipeline(*s,fb,db,perf_);
    s->cl=s->present_ring->end(s->present_cb);
    s->q.submitCommands(s->cl);
    s->cl=0;
  }
  if(perf_)perf_->recordRenderSubmit(toMicroseconds(RenderClock::now()-render_start));
  if(shouldLogRender())lunar::diagnosticLog("render","present submit width=%d height=%d",s->frame_w,s->frame_h);
}

bool VideoRenderer::pollEvents(){return true;}

void VideoRenderer::shutdown(){
  {
    std::lock_guard<std::mutex> lock(software_mutex_);
    if(software_sws_){
      auto* sws=reinterpret_cast<SwsContext*>(software_sws_);
      sws_freeContext(sws);
      software_sws_=nullptr;
    }
    software_rgba_.clear();
  }
  SoftwareVideoFrameSink::instance().clear();
  auto* s=static_cast<Deko3DRenderContext*>(ctx_);if(!s)return;
  if(!s->ok && !s->vctx && !s->pc && !s->pd && !s->pi)return;
  if(s->q)s->q.waitIdle();
  releaseRetainedFrames(*s);
  s->fms.clear();
  releasePostProcessTargets(*s);
  s->direct_cb.clear();
  s->direct_cl=0;
  s->direct_cmd_mem.destroy();
  s->vertex_buf.destroy();
  s->easu_uniform.destroy();
  s->rcas_uniform.destroy();
  s->dithering_uniform.destroy();
  s->tu.destroy();
  if(s->vctx&&s->li>=0){s->vctx->freeImageIndex(s->li);s->li=-1;}
  if(s->vctx&&s->ci>=0){s->vctx->freeImageIndex(s->ci);s->ci=-1;}
  if(s->vctx&&s->source_target.texture_slot>=0){s->vctx->freeImageIndex(s->source_target.texture_slot);s->source_target.texture_slot=-1;}
  if(s->vctx&&s->upscale_target.texture_slot>=0){s->vctx->freeImageIndex(s->upscale_target.texture_slot);s->upscale_target.texture_slot=-1;}
  if(s->vctx&&s->rcas_target.texture_slot>=0){s->vctx->freeImageIndex(s->rcas_target.texture_slot);s->rcas_target.texture_slot=-1;}
  s->update_cb={};
  s->present_cb={};
  s->direct_cb={};
  s->update_ring.reset();
  s->present_ring.reset();
  s->pd.reset();
  s->pc.reset();
  s->pi.reset();
  s->vctx=nullptr;
  s->dev=nullptr;
  s->q=nullptr;
  configurePresentPipeline(s->pipeline, PostProcessMode::Off);
  s->static_state_dirty=true;
  s->ok=false;
}

}
#else
#include <SDL2/SDL.h>
#include <mutex>
namespace lunar::stream {
VideoRenderer::VideoRenderer()=default;VideoRenderer::~VideoRenderer(){shutdown();}
void VideoRenderer::setVideoBackend(VideoBackend){video_backend_=VideoBackend::Software;}
void VideoRenderer::setPostProcessMode(PostProcessMode){}
void VideoRenderer::setPostProcessEnabled(bool){}
void VideoRenderer::setDitheringEnabled(bool, float){}
bool VideoRenderer::initialize(const char* t,int w,int h){
  if(SDL_Init(SDL_INIT_VIDEO)<0)return false;
  auto* win=SDL_CreateWindow(t,SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,w,h,SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
  if(!win)return false;window_=win;
  auto* r=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if(!r)return false;renderer_=r;
  auto* tx=SDL_CreateTexture(r,SDL_PIXELFORMAT_IYUV,SDL_TEXTUREACCESS_STREAMING,w,h);
  if(!tx)return false;texture_=tx;return true;
}
bool VideoRenderer::render(const VideoFrame& f){
  std::lock_guard<std::mutex> lock(sdl_mutex_);
  auto* t=static_cast<SDL_Texture*>(texture_);
  auto* r=static_cast<SDL_Renderer*>(renderer_);
  if(!t||!r)return false;
  SDL_Rect rect = {0, 0, f.width, f.height};
  SDL_UpdateYUVTexture(t, &rect, f.data[0], f.linesize[0], f.data[1], f.linesize[1], f.data[2], f.linesize[2]);
  SDL_RenderClear(r);SDL_RenderCopy(r,t,nullptr,nullptr);SDL_RenderPresent(r);return true;
}
void VideoRenderer::present(){}
bool VideoRenderer::pollEvents(){SDL_Event e;while(SDL_PollEvent(&e))if(e.type==SDL_QUIT||(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE))return false;return true;}
void VideoRenderer::shutdown(){
  std::lock_guard<std::mutex> lock(sdl_mutex_);
  if(texture_){SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));texture_=nullptr;}
  if(renderer_){SDL_DestroyRenderer(static_cast<SDL_Renderer*>(renderer_));renderer_=nullptr;}
  if(window_){SDL_DestroyWindow(static_cast<SDL_Window*>(window_));window_=nullptr;}
}
}
#endif
