#include "video_renderer.h"
#include "../diagnostics.h"
#include "perf_stats.h"
#include "software_video_frame.h"
#include "video_resolution_transition.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
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
  static constexpr size_t MaxRetiredTargets =
      brls::FRAMEBUFFERS_COUNT * 6;
  // Real-hardware diagnostics are synchronously flushed to the SD card. Keep
  // success-path probes tiny: the old first-24 + every-120-frame trace caused
  // visible periodic stalls even though decode and presentation were healthy.
  static constexpr int RenderLogLimit = 8;
  using RenderClock = std::chrono::steady_clock;
  std::atomic<int> render_logs{0};

  bool shouldLogRender() {
    return render_logs.fetch_add(1) < RenderLogLimit;
  }

  bool shouldLogHardwareProbe(uint64_t frame_id) {
    return frame_id == 1;
  }

  bool shouldLogCloud1080HardwareProbe(uint64_t frame_id,
                                       const char* stage) {
    if (!lunar::cloud1080CrashProbeEnabled() || frame_id == 0 || !stage) {
      return false;
    }
    if (frame_id == 1) return true;
    const bool coarse_stage = std::strcmp(stage, "present-entry") == 0 ||
        std::strcmp(stage, "mapping-ready") == 0 ||
        std::strcmp(stage, "mapping-rejected") == 0 ||
        std::strcmp(stage, "queue-submit-after") == 0;
    return coarse_stage && lunar::shouldSampleCloud1080CrashProbe(frame_id);
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
    uint32_t sz=0,lo=0,co=0;
    int w=0,hgt=0;
    uint32_t lw=0,lh=0,cw=0,chh=0;
    bool linear=false;
    dk::ImageLayout ll{},cll{};
    dk::UniqueMemBlock mb{};
    dk::Image lu{},ch{};
    dk::ImageDescriptor ld{},cd{};

    bool matches(uint32_t handle, void* addr, uint32_t size,
                 uint32_t luma_offset, uint32_t chroma_offset,
                 int width, int height, bool is_linear,
                 uint32_t luma_width, uint32_t luma_height,
                 uint32_t chroma_width, uint32_t chroma_height) const {
      return h == handle && a == addr && sz == size && lo == luma_offset &&
             co == chroma_offset && w == width && hgt == height &&
             linear == is_linear && lw == luma_width && lh == luma_height &&
             cw == chroma_width && chh == chroma_height;
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

  struct RetiredTarget {
    CMemPool::Handle handle;
    uint32_t pending_slice_mask = 0;
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
  DkCmdList cl=0;
  CShader vs,fs,blit_fs,upscaling_fs,rcas_fs;
  CMemPool::Handle tu;
  CMemPool::Handle easu_uniform;
  CMemPool::Handle rcas_uniform;
  CMemPool::Handle dithering_uniform;
  CMemPool::Handle vertex_buf;
  std::vector<FrameMapping> fms;
  int fi=-1;
  int li=-1,ci=-1;
  std::optional<CCmdMemRing<brls::FRAMEBUFFERS_COUNT>> update_ring;
  std::optional<CCmdMemRing<brls::FRAMEBUFFERS_COUNT>> present_ring;
  RenderTarget source_target;
  RenderTarget upscale_target;
  RenderTarget rcas_target;
  int frame_w=1280, frame_h=720;
  uint32_t mapped_luma_w=0, mapped_luma_h=0;
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
  std::condition_variable decoder_reset_cv;
  bool decoder_reset_requested=false;
  bool decoder_reset_ready=false;
  size_t decoder_reset_drain_steps=0;
  VideoResolutionTransition resolution_transition;
  AVFrame* resolution_transition_frame=nullptr;
  size_t resolution_transition_drain_steps=0;
  AVFrame* pending_frame=nullptr;
  AVFrame* current_frame=nullptr;
  std::array<AVFrame*, brls::FRAMEBUFFERS_COUNT> submitted_frames{};
  size_t next_submitted_frame=0;
  std::vector<RetiredTarget> retired_targets;
  size_t active_present_slice=0;
  bool present_slice_active=false;
  uint64_t present_frame_id=0;
};

namespace {

void hardwareProbeLog(uint64_t frame_id, const char* stage,
                      const char* format = nullptr, ...) {
  const bool crash_probe = shouldLogCloud1080HardwareProbe(frame_id, stage);
  if (!crash_probe && !shouldLogHardwareProbe(frame_id)) return;

  char details[384] = {};
  if (format) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(details, sizeof(details), format, args);
    va_end(args);
  }
  if (crash_probe) {
    lunar::cloud1080CrashProbeLog(
        "crash-probe",
        "DEBUG-c1080 phase=present frame=%llu stage=%s%s%s",
        static_cast<unsigned long long>(frame_id),
        stage ? stage : "?",
        format ? " " : "",
        format ? details : "");
  } else {
    lunar::diagnosticLog(
        "render-hwdiag",
        "frame=%llu stage=%s%s%s",
        static_cast<unsigned long long>(frame_id),
        stage ? stage : "?",
        format ? " " : "",
        format ? details : "");
  }
}

void releaseRetainedFrames(Deko3DRenderContext& s) {
  if(s.resolution_transition_frame)av_frame_free(&s.resolution_transition_frame);
  if(s.pending_frame)av_frame_free(&s.pending_frame);
  if(s.current_frame)av_frame_free(&s.current_frame);
  for(auto*& frame:s.submitted_frames){
    if(frame)av_frame_free(&frame);
  }
  s.next_submitted_frame=0;
}

uint64_t renderNowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          RenderClock::now().time_since_epoch()).count());
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

constexpr uint32_t allPresentSlicesMask() {
  static_assert(brls::FRAMEBUFFERS_COUNT > 0 &&
                brls::FRAMEBUFFERS_COUNT <= 32,
                "present slice count must fit the retirement mask");
  if constexpr (brls::FRAMEBUFFERS_COUNT == 32) {
    return UINT32_MAX;
  } else {
    return (uint32_t{1} << brls::FRAMEBUFFERS_COUNT) - 1u;
  }
}

void retireCompletedTargets(Deko3DRenderContext& s, size_t slice) {
  if (slice >= brls::FRAMEBUFFERS_COUNT) return;
  const uint32_t completed_bit = uint32_t{1} << slice;
  for (size_t i = 0; i < s.retired_targets.size();) {
    auto& retired = s.retired_targets[i];
    retired.pending_slice_mask &= ~completed_bit;
    if (retired.pending_slice_mask == 0) {
      retired.handle.destroy();
      s.retired_targets.erase(s.retired_targets.begin() + i);
      continue;
    }
    ++i;
  }
}

bool retireRenderTarget(Deko3DRenderContext& s, RenderTarget& target) {
  if (!target.handle) return true;
  if (s.retired_targets.size() >= MaxRetiredTargets) {
    lunar::diagnosticLog("render",
                         "post-process target retirement limit reached; keeping target alive");
    return false;
  }

  uint32_t pending_mask = allPresentSlicesMask();
  if (s.present_slice_active) {
    // present_ring->begin() already waited for this slice before recording the
    // current command list. The old target is not referenced by the new list,
    // so this slice is already safe to retire.
    pending_mask &= ~(uint32_t{1} << s.active_present_slice);
  }

  try {
    s.retired_targets.push_back({target.handle, pending_mask});
  } catch (...) {
    lunar::diagnosticLog("render",
                         "post-process target retirement allocation failed; keeping target alive");
    return false;
  }

  target.handle = CMemPool::Handle{};
  return true;
}

bool deferResetRenderTarget(Deko3DRenderContext& s, RenderTarget& target) {
  if (!retireRenderTarget(s, target)) return false;
  // The image metadata is invalid once its backing handle has been queued for
  // retirement, even though the handle itself remains alive in the queue.
  target.layout = dk::ImageLayout{};
  target.image = dk::Image{};
  target.descriptor = dk::ImageDescriptor{};
  target.width = 0;
  target.height = 0;
  target.descriptor_current = false;
  return true;
}

bool allocateRenderTarget(Deko3DRenderContext& s, RenderTarget& target,
                          int width, int height) {
  if (!s.pi || target.texture_slot < 0 || width <= 0 || height <= 0) return false;
  if (target.allocated() && target.width == width && target.height == height) {
    return true;
  }

  RenderTarget replacement;
  replacement.texture_slot = target.texture_slot;
  dk::ImageLayoutMaker{s.dev}
      .setType(DkImageType_2D)
      .setFormat(DkImageFormat_RGBA8_Unorm)
      .setDimensions(width, height, 1)
      .setFlags(DkImageFlags_UsageRender |
                DkImageFlags_UsageLoadStore |
                DkImageFlags_Usage2DEngine)
      .initialize(replacement.layout);

  replacement.handle = s.pi->allocate(replacement.layout.getSize(),
                                      replacement.layout.getAlignment());
  if (!replacement.handle) return false;

  replacement.image.initialize(replacement.layout,
                               replacement.handle.getMemBlock(),
                               replacement.handle.getOffset());
  replacement.descriptor.initialize(replacement.image);
  replacement.width = width;
  replacement.height = height;
  replacement.descriptor_current = false;

  if (!deferResetRenderTarget(s, target)) {
    resetRenderTarget(replacement);
    return false;
  }
  target = replacement;
  replacement.handle = CMemPool::Handle{};
  return true;
}

bool updateRenderTargetDescriptor(Deko3DRenderContext& s, RenderTarget& target) {
  if (!target.allocated() || target.texture_slot < 0) return false;
  hardwareProbeLog(s.present_frame_id, "target-descriptor-begin",
                   "slot=%d size=%dx%d queue_error=%d",
                   target.texture_slot, target.width, target.height,
                   s.q && s.q.isInErrorState() ? 1 : 0);
  s.update_ring->begin(s.update_cb);
  bool updated = s.vctx->updateImageDescriptor(s.update_cb,
                                               target.texture_slot,
                                               target.descriptor);
  if (updated) s.vctx->invalidateImageDescriptors(s.update_cb);
  s.q.submitCommands(s.update_ring->end(s.update_cb));
  target.descriptor_current = updated;
  hardwareProbeLog(s.present_frame_id, "target-descriptor-submit",
                   "slot=%d updated=%d queue_error=%d",
                   target.texture_slot, updated ? 1 : 0,
                   s.q && s.q.isInErrorState() ? 1 : 0);
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
    deferResetRenderTarget(s, target);
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
    deferResetRenderTarget(s, s.source_target);
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
    deferResetRenderTarget(s, s.rcas_target);
    return false;
  }
  if (!ensurePostProcessTarget(s, s.rcas_target, s.target_w, s.target_h, "rcas")) {
    fprintf(stderr, "[render] RCAS target unavailable, falling back to base post pass\n");
    deferResetRenderTarget(s, s.rcas_target);
    return false;
  }
  return true;
}

void releasePostProcessTargets(Deko3DRenderContext& s) {
  deferResetRenderTarget(s, s.source_target);
  deferResetRenderTarget(s, s.upscale_target);
  deferResetRenderTarget(s, s.rcas_target);
}

void destroyRetiredTargets(Deko3DRenderContext& s) {
  for (auto& retired : s.retired_targets) retired.handle.destroy();
  s.retired_targets.clear();
}

void destroyPostProcessTargets(Deko3DRenderContext& s) {
  resetRenderTarget(s.source_target);
  resetRenderTarget(s.upscale_target);
  resetRenderTarget(s.rcas_target);
  destroyRetiredTargets(s);
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

Tf cropToMappedSurface(const Deko3DRenderContext& s, Tf transform) {
  if(s.frame_w<=0||s.frame_h<=0||s.mapped_luma_w==0||s.mapped_luma_h==0){
    return transform;
  }
  const float scale_x=static_cast<float>(s.frame_w)/
      static_cast<float>(s.mapped_luma_w);
  const float scale_y=static_cast<float>(s.frame_h)/
      static_cast<float>(s.mapped_luma_h);
  transform.u[0]*=scale_x;
  transform.u[1]*=scale_y;
  transform.u[2]*=scale_x;
  transform.u[3]*=scale_y;
  return transform;
}

void recordNv12ToFramebufferPass(Deko3DRenderContext& s, const Tf& transform) {
  s.present_cb.bindShaders(DkStageFlag_GraphicsMask,{s.vs,s.fs});
  s.present_cb.bindTextures(DkStage_Fragment,0,dkMakeTextureHandle(s.li,0));
  s.present_cb.bindTextures(DkStage_Fragment,1,dkMakeTextureHandle(s.ci,0));
  s.present_cb.bindUniformBuffer(DkStage_Fragment,0,s.tu.getGpuAddr(),s.tu.getSize());
  const Tf mapped_transform=cropToMappedSurface(s,transform);
  s.present_cb.pushConstants(s.tu.getGpuAddr(),sizeof(Tf),0,sizeof(Tf),&mapped_transform);
  s.present_cb.draw(DkPrimitive_Quads,kQuadVertices.size(),1,0,0);
}

void updateDitheringUniform(Deko3DRenderContext& s, bool enabled) {
  if (!s.dithering_uniform) return;
  DitheringConstants constants{};
  constants.control[0] = enabled ? 1.0f : 0.0f;
  constants.control[1] = std::clamp(s.dithering_strength, 1.0f, 10.0f);
  s.present_cb.pushConstants(s.dithering_uniform.getGpuAddr(),
                             s.dithering_uniform.getSize(),
                             0, sizeof(constants), &constants);
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
  s.present_cb.pushConstants(s.easu_uniform.getGpuAddr(),
                             s.easu_uniform.getSize(),
                             0, sizeof(constants), &constants);
}

void updateRcasUniform(Deko3DRenderContext& s) {
  RcasConstants constants = {};
  populateRcasConstants(constants, s.rcas_strength);
  s.present_cb.pushConstants(s.rcas_uniform.getGpuAddr(),
                             s.rcas_uniform.getSize(),
                             0, sizeof(constants), &constants);
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
      deferResetRenderTarget(s, s.rcas_target);
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

bool updateFrameMapping(Deko3DRenderContext& s, AVFrame* frame) {
  if(!frame||frame->format!=AV_PIX_FMT_NVTEGRA)return false;
  hardwareProbeLog(s.present_frame_id, "mapping-begin",
                   "format=%d size=%dx%d queue_error=%d",
                   frame->format, frame->width, frame->height,
                   s.q && s.q.isInErrorState() ? 1 : 0);
  AVNVTegraMap* nvmap=av_nvtegra_frame_get_fbuf_map(frame);
  if(!nvmap){
    lunar::diagnosticLog("render","present reject missing nvtegra map width=%d height=%d",frame->width,frame->height);
    return false;
  }

  const uint32_t handle=av_nvtegra_map_get_handle(nvmap);
  void* const address=av_nvtegra_map_get_addr(nvmap);
  const uint32_t size=av_nvtegra_map_get_size(nvmap);
  hardwareProbeLog(s.present_frame_id, "mapping-map-info",
                   "handle=%u base=%p bytes=%u linear=%d y=%p uv=%p pitch=%d/%d",
                   handle, address, size, nvmap->is_linear ? 1 : 0,
                   frame->data[0], frame->data[1], frame->linesize[0],
                   frame->linesize[1]);
  if(!handle||!address||size==0||!frame->data[0]||!frame->data[1]||
     frame->linesize[0]<=0||frame->linesize[1]<=0){
    lunar::diagnosticLog("render","present reject invalid nvtegra map handle=%u addr=%p size=%u",handle,address,size);
    return false;
  }

  const uintptr_t base_addr=reinterpret_cast<uintptr_t>(address);
  const uintptr_t luma_addr=reinterpret_cast<uintptr_t>(frame->data[0]);
  const uintptr_t chroma_addr=reinterpret_cast<uintptr_t>(frame->data[1]);
  if(luma_addr<base_addr||chroma_addr<base_addr){
    lunar::diagnosticLog("render","present reject nvtegra planes before map handle=%u addr=%p y=%p uv=%p",handle,address,frame->data[0],frame->data[1]);
    return false;
  }
  const uint64_t luma_delta=static_cast<uint64_t>(luma_addr-base_addr);
  const uint64_t chroma_delta=static_cast<uint64_t>(chroma_addr-base_addr);
  if(luma_delta>=size||chroma_delta>=size||chroma_delta<=luma_delta||
     luma_delta>UINT32_MAX||chroma_delta>UINT32_MAX){
    lunar::diagnosticLog("render","present reject nvtegra plane offsets handle=%u size=%u y=%llu uv=%llu",handle,size,(unsigned long long)luma_delta,(unsigned long long)chroma_delta);
    return false;
  }
  const uint32_t luma_offset=static_cast<uint32_t>(luma_delta);
  const uint32_t chroma_offset=static_cast<uint32_t>(chroma_delta);
  const bool is_linear=nvmap->is_linear;
  const auto align_up=[](uint32_t value,uint32_t alignment){
    return (value+alignment-1u)&~(alignment-1u);
  };
  const uint32_t luma_width=std::max<uint32_t>(
      static_cast<uint32_t>(frame->linesize[0]),static_cast<uint32_t>(frame->width));
  const uint32_t luma_height=align_up(static_cast<uint32_t>(frame->height),32u);
  const uint32_t chroma_width=std::max<uint32_t>(
      static_cast<uint32_t>((frame->linesize[1]+1)/2),
      static_cast<uint32_t>((frame->width+1)/2));
  const uint32_t chroma_height=align_up(static_cast<uint32_t>(frame->height+1)/2u,16u);
  if(frame->width<=0||frame->height<=0||luma_width==0||luma_height==0||
     chroma_width==0||chroma_height==0){
    lunar::diagnosticLog("render","present reject invalid nvtegra geometry width=%d height=%d pitches=%d/%d",frame->width,frame->height,frame->linesize[0],frame->linesize[1]);
    return false;
  }

  if(frame->width!=s.frame_w||frame->height!=s.frame_h){
    s.static_state_dirty=true;
  }

  int mapping_index=-1;
  for(size_t i=0;i<s.fms.size();++i){
    if(s.fms[i].matches(handle,address,size,luma_offset,chroma_offset,
                        frame->width,frame->height,is_linear,
                        luma_width,luma_height,chroma_width,chroma_height)){
      mapping_index=static_cast<int>(i);
      break;
    }
  }

  if(mapping_index<0){
    if(s.fms.size()>=32){
      lunar::diagnosticLog("render","present reject nvtegra mapping cache full size=%zu",s.fms.size());
      return false;
    }

    FrameMapping mapping;
    mapping.h=handle;
    mapping.a=address;
    mapping.sz=size;
    mapping.lo=luma_offset;
    mapping.co=chroma_offset;
    mapping.w=frame->width;
    mapping.hgt=frame->height;
    mapping.lw=luma_width;
    mapping.lh=luma_height;
    mapping.cw=chroma_width;
    mapping.chh=chroma_height;
    mapping.linear=is_linear;

    const uint32_t layout_flags=DkImageFlags_UsageLoadStore|
        DkImageFlags_Usage2DEngine|DkImageFlags_UsageVideo|
        (is_linear?DkImageFlags_PitchLinear:DkImageFlags_CustomTileSize);
    dk::ImageLayoutMaker luma_maker{s.dev};
    luma_maker.setType(DkImageType_2D)
        .setFormat(DkImageFormat_R8_Unorm)
        .setDimensions(luma_width,luma_height,1)
        .setFlags(layout_flags);
    dk::ImageLayoutMaker chroma_maker{s.dev};
    chroma_maker.setType(DkImageType_2D)
        .setFormat(DkImageFormat_RG8_Unorm)
        .setDimensions(chroma_width,chroma_height,1)
        .setFlags(layout_flags);
    if(is_linear){
      luma_maker.setPitchStride(static_cast<uint32_t>(frame->linesize[0]));
      chroma_maker.setPitchStride(static_cast<uint32_t>(frame->linesize[1]));
    }else{
      luma_maker.setTileSize(DkTileSize_TwoGobs);
      chroma_maker.setTileSize(DkTileSize_TwoGobs);
    }
    luma_maker.initialize(mapping.ll);
    chroma_maker.initialize(mapping.cll);
    hardwareProbeLog(s.present_frame_id, "mapping-layout",
                     "handle=%u y_off=%u uv_off=%u y_layout=%llu uv_layout=%llu "
                     "y_align=%u uv_align=%u base_align=%u size_align=%u",
                     handle, luma_offset, chroma_offset,
                     static_cast<unsigned long long>(mapping.ll.getSize()),
                     static_cast<unsigned long long>(mapping.cll.getSize()),
                     mapping.ll.getAlignment(), mapping.cll.getAlignment(),
                     static_cast<unsigned>(reinterpret_cast<uintptr_t>(address) & 0xfff),
                     size & 0xfff);
    const uint64_t luma_end=static_cast<uint64_t>(luma_offset)+mapping.ll.getSize();
    const uint64_t chroma_end=static_cast<uint64_t>(chroma_offset)+mapping.cll.getSize();
    if(luma_end>size||chroma_end>size){
      lunar::diagnosticLog("render","present reject nvtegra layout handle=%u size=%u y=%u uv=%u luma_layout=%llu chroma_layout=%llu",handle,size,luma_offset,chroma_offset,(unsigned long long)mapping.ll.getSize(),(unsigned long long)mapping.cll.getSize());
      return false;
    }

    hardwareProbeLog(s.present_frame_id, "mapping-memblock-before",
                     "handle=%u base=%p size=%u y_off=%u uv_off=%u",
                     handle, address, size, luma_offset, chroma_offset);
    auto external_mem=dk::MemBlockMaker{s.dev,size}
        .setFlags(DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached|DkMemBlockFlags_Image)
        .setStorage(address)
        .create();
    hardwareProbeLog(s.present_frame_id, "mapping-memblock-after",
                     "handle=%u created=%d queue_error=%d",
                     handle, external_mem ? 1 : 0,
                     s.q && s.q.isInErrorState() ? 1 : 0);
    if(!external_mem){
      lunar::diagnosticLog("render","present reject external memblock handle=%u addr=%p size=%u",handle,address,size);
      return false;
    }

    mapping.mb=dk::UniqueMemBlock{std::move(external_mem)};
    mapping.lu.initialize(mapping.ll,mapping.mb,luma_offset);
    mapping.ch.initialize(mapping.cll,mapping.mb,chroma_offset);
    mapping.ld.initialize(mapping.lu);
    mapping.cd.initialize(mapping.ch);
    try{
      mapping_index=static_cast<int>(s.fms.size());
      s.fms.push_back(std::move(mapping));
    }catch(...){
      lunar::diagnosticLog("render","present reject nvtegra mapping allocation failed handle=%u size=%u",handle,size);
      return false;
    }
    if(shouldLogRender()){
      lunar::diagnosticLog("render","nvtegra mapping added index=%d handle=%u addr=%p size=%u linear=%d y=%u uv=%u pitches=%d/%d layouts=%llu/%llu",mapping_index,handle,address,size,is_linear?1:0,luma_offset,chroma_offset,frame->linesize[0],frame->linesize[1],(unsigned long long)s.fms[mapping_index].ll.getSize(),(unsigned long long)s.fms[mapping_index].cll.getSize());
    }
  }

  if(mapping_index!=s.fi){
    hardwareProbeLog(s.present_frame_id, "plane-descriptor-begin",
                     "mapping=%d previous=%d luma_slot=%d chroma_slot=%d queue_error=%d",
                     mapping_index, s.fi, s.li, s.ci,
                     s.q && s.q.isInErrorState() ? 1 : 0);
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
    hardwareProbeLog(s.present_frame_id, "plane-descriptor-submit",
                     "mapping=%d luma_ok=%d chroma_ok=%d queue_error=%d",
                     mapping_index, luma_ok ? 1 : 0, chroma_ok ? 1 : 0,
                     s.q && s.q.isInErrorState() ? 1 : 0);
  }

  const auto& active_mapping=s.fms[mapping_index];
  s.mapped_luma_w=active_mapping.lw;
  s.mapped_luma_h=active_mapping.lh;
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
    s->present_frame_id=0;
    s->update_ring.emplace();
    s->present_ring.emplace();
    s->retired_targets.reserve(MaxRetiredTargets);
    if(!s->update_ring->allocate(*s->pd,UpdateCmdSliceSize)||
       !s->present_ring->allocate(*s->pd,PresentCmdSliceSize)){
      fprintf(stderr,"[render] cmd ring alloc fail\n");return false;
    }
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
  s->resolution_transition.configure(w,h);
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
  const bool had_startup_candidate=
      s->resolution_transition.hasStartupCandidate();
  const int previous_candidate_width=
      s->resolution_transition.candidateWidth();
  const int previous_candidate_height=
      s->resolution_transition.candidateHeight();
  const auto decision=s->resolution_transition.observeFrame(
      f->width,f->height,renderNowMs());
  if(decision==ResolutionFrameDecision::HoldStartup){
    if(s->resolution_transition_frame)
      av_frame_free(&s->resolution_transition_frame);
    s->resolution_transition_frame=keep;
    if(!had_startup_candidate||f->width!=previous_candidate_width||
       f->height!=previous_candidate_height){
      lunar::dropDiagnosticLog(
          "resolution-transition",
          "phase=startup-hold candidate=%dx%d target=%dx%d wait_ms=%llu",
          f->width,f->height,s->resolution_transition.targetWidth(),
          s->resolution_transition.targetHeight(),
          static_cast<unsigned long long>(
              VideoResolutionTransition::StartupHoldMs));
    }
    return true;
  }
  if(decision==ResolutionFrameDecision::BeginTransition||
     decision==ResolutionFrameDecision::HoldTransition){
    if(s->resolution_transition_frame)
      av_frame_free(&s->resolution_transition_frame);
    s->resolution_transition_frame=keep;
    if(decision==ResolutionFrameDecision::BeginTransition){
      s->resolution_transition_drain_steps=0;
      lunar::dropDiagnosticLog(
          "resolution-transition",
          "phase=start old=%dx%d new=%dx%d",
          s->resolution_transition.activeWidth(),
          s->resolution_transition.activeHeight(),f->width,f->height);
    }
    return true;
  }
  if(decision==ResolutionFrameDecision::KeepCurrent){
    av_frame_free(&keep);
    return true;
  }
  if(s->resolution_transition_frame)
    av_frame_free(&s->resolution_transition_frame);
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
  if(s->decoder_reset_requested){
    if(!s->q||!s->present_ring){
      s->decoder_reset_ready=false;
      s->decoder_reset_requested=false;
      s->decoder_reset_drain_steps=0;
      s->decoder_reset_cv.notify_all();
      return;
    }

    const size_t submitted_index=s->next_submitted_frame;
    s->present_ring->begin(s->present_cb);
    retireCompletedTargets(*s,submitted_index);
    auto*& completed_frame=s->submitted_frames[submitted_index];
    if(completed_frame)av_frame_free(&completed_frame);
    const DkCmdList reset_list=s->present_ring->end(s->present_cb);
    s->q.submitCommands(reset_list);
    s->next_submitted_frame=
        (submitted_index+1)%s->submitted_frames.size();
    s->present_slice_active=false;
    s->decoder_reset_drain_steps++;

    // One pass retires every video command-ring slice. Beginning the first
    // slice once more waits for the first no-op fence, which also proves that
    // descriptor updates submitted before reset have completed. This avoids
    // waiting the entire Borealis queue from inside draw().
    if(s->decoder_reset_drain_steps>s->submitted_frames.size()){
      s->fms.clear();
      s->fi=-1;
      s->mapped_luma_w=0;
      s->mapped_luma_h=0;
      s->static_state_dirty=true;
      if(s->pending_frame)av_frame_free(&s->pending_frame);
      if(s->current_frame)av_frame_free(&s->current_frame);
      if(s->resolution_transition_frame)
        av_frame_free(&s->resolution_transition_frame);
      s->resolution_transition.reset();
      s->resolution_transition_drain_steps=0;
      s->decoder_reset_ready=true;
      s->decoder_reset_requested=false;
      s->decoder_reset_drain_steps=0;
      s->decoder_reset_cv.notify_all();
      lunar::dropDiagnosticLog(
          "video-reset", "phase=gpu-fences-retired action=decoder-flush-ready");
    }
    return;
  }
  if(s->resolution_transition.startupCandidateReady(renderNowMs())){
    if(s->pending_frame)av_frame_free(&s->pending_frame);
    s->pending_frame=s->resolution_transition_frame;
    s->resolution_transition_frame=nullptr;
    s->resolution_transition.promoteStartupCandidate();
    lunar::dropDiagnosticLog(
        "resolution-transition",
        "phase=startup-promote candidate=%dx%d",
        s->resolution_transition.activeWidth(),
        s->resolution_transition.activeHeight());
  }
  if(s->resolution_transition.isTransitioning()){
    if(!s->q||!s->present_ring)return;

    const size_t submitted_index=s->next_submitted_frame;
    s->present_ring->begin(s->present_cb);
    retireCompletedTargets(*s,submitted_index);
    auto*& completed_frame=s->submitted_frames[submitted_index];
    if(completed_frame)av_frame_free(&completed_frame);
    const DkCmdList transition_list=s->present_ring->end(s->present_cb);
    s->q.submitCommands(transition_list);
    s->next_submitted_frame=
        (submitted_index+1)%s->submitted_frames.size();
    s->present_slice_active=false;
    s->resolution_transition_drain_steps++;

    if(s->resolution_transition_drain_steps>s->submitted_frames.size()){
      const int old_width=s->resolution_transition.activeWidth();
      const int old_height=s->resolution_transition.activeHeight();
      const int new_width=s->resolution_transition.candidateWidth();
      const int new_height=s->resolution_transition.candidateHeight();

      // Every old command slice is fenced at this point. Destroy external
      // Deko3D wrappers before releasing the NVDEC frames that own them.
      s->fms.clear();
      s->fi=-1;
      s->mapped_luma_w=0;
      s->mapped_luma_h=0;
      s->static_state_dirty=true;
      if(s->pending_frame)av_frame_free(&s->pending_frame);
      if(s->current_frame)av_frame_free(&s->current_frame);
      s->pending_frame=s->resolution_transition_frame;
      s->resolution_transition_frame=nullptr;
      s->resolution_transition.completeTransition();
      s->resolution_transition_drain_steps=0;
      lunar::dropDiagnosticLog(
          "resolution-transition",
          "phase=fences-retired old=%dx%d new=%dx%d",
          old_width,old_height,new_width,new_height);
    }
    return;
  }
  if(!s->pending_frame&&!s->current_frame){
    return;
  }
  const uint64_t frame_id=++s->present_frame_id;
  hardwareProbeLog(frame_id, "present-entry",
                   "pending=%d current=%d queue_error=%d",
                   s->pending_frame ? 1 : 0, s->current_frame ? 1 : 0,
                   s->q && s->q.isInErrorState() ? 1 : 0);
  if(s->pending_frame){
    if(s->current_frame)av_frame_free(&s->current_frame);
    s->current_frame=s->pending_frame;
    s->pending_frame=nullptr;
  }
  if(!updateFrameMapping(*s,s->current_frame)){
    hardwareProbeLog(frame_id, "mapping-rejected",
                     "queue_error=%d", s->q && s->q.isInErrorState() ? 1 : 0);
    return;
  }
  hardwareProbeLog(frame_id, "mapping-ready",
                   "frame=%dx%d mapped=%ux%u queue_error=%d",
                   s->frame_w, s->frame_h, s->mapped_luma_w,
                   s->mapped_luma_h, s->q && s->q.isInErrorState() ? 1 : 0);

  dk::Image* fb = s->vctx->getFramebuffer();
  dk::Image* db = s->vctx->getDepthBuffer();
  hardwareProbeLog(frame_id, "framebuffer-acquire",
                   "fb=%p db=%p queue_error=%d", fb, db,
                   s->q && s->q.isInErrorState() ? 1 : 0);
  if(!fb||!db){
    if(shouldLogRender())lunar::diagnosticLog("render","present reject framebuffer=%s depth=%s",fb?"true":"false",db?"true":"false");
    return;
  }

  if(!s->present_ring)return;
  hardwareProbeLog(frame_id, "command-ring-begin-before",
                   "slice=%zu queue_error=%d", s->next_submitted_frame,
                   s->q && s->q.isInErrorState() ? 1 : 0);
#if LUNARNX_DROP_DIAGNOSTIC_LOG
  const auto present_wait_start=RenderClock::now();
#endif
  s->present_ring->begin(s->present_cb);
#if LUNARNX_DROP_DIAGNOSTIC_LOG
  if(perf_)perf_->recordPresentWait(
      toMicroseconds(RenderClock::now()-present_wait_start));
#endif
  hardwareProbeLog(frame_id, "command-ring-begin-after",
                   "slice=%zu queue_error=%d", s->next_submitted_frame,
                   s->q && s->q.isInErrorState() ? 1 : 0);
  const size_t submitted_index=s->next_submitted_frame;
  retireCompletedTargets(*s, submitted_index);
  s->active_present_slice=submitted_index;
  s->present_slice_active=true;
  auto*& completed_frame=s->submitted_frames[submitted_index];
  // CCmdMemRing::begin() waits for this command slice's fence.  The matching
  // retained AVFrame can therefore be released without blocking the
  // Borealis beginFrame/endFrame swapchain lifecycle.
  if(completed_frame)av_frame_free(&completed_frame);

  AVFrame* submitted_frame=av_frame_alloc();
  if(!submitted_frame||av_frame_ref(submitted_frame,s->current_frame)<0){
    if(submitted_frame)av_frame_free(&submitted_frame);
    s->present_slice_active=false;
    if(shouldLogRender())lunar::diagnosticLog("render","present reject in-flight frame ref failed");
    return;
  }

  const auto render_start = RenderClock::now();
  hardwareProbeLog(frame_id, "record-pipeline-begin",
                   "fb=%p db=%p static_dirty=%d queue_error=%d",
                   fb, db, s->static_state_dirty ? 1 : 0,
                   s->q && s->q.isInErrorState() ? 1 : 0);
  recordPresentPipeline(*s,fb,db,perf_);
  hardwareProbeLog(frame_id, "record-pipeline-end",
                   "pipeline_passes=%zu target=%dx%d queue_error=%d",
                   s->pipeline.pass_count, s->target_w, s->target_h,
                   s->q && s->q.isInErrorState() ? 1 : 0);
  s->cl=s->present_ring->end(s->present_cb);
  hardwareProbeLog(frame_id, "finish-list-end",
                   "cmdlist=%p queue_error=%d", reinterpret_cast<void*>(s->cl),
                   s->q && s->q.isInErrorState() ? 1 : 0);
  s->q.submitCommands(s->cl);
  hardwareProbeLog(frame_id, "queue-submit-after",
                   "slice=%zu queue_error=%d", submitted_index,
                   s->q && s->q.isInErrorState() ? 1 : 0);
  s->cl=0;
  completed_frame=submitted_frame;
  s->next_submitted_frame=(submitted_index+1)%s->submitted_frames.size();
  s->present_slice_active=false;
  if(perf_)perf_->recordRenderSubmit(toMicroseconds(RenderClock::now()-render_start));
  if(shouldLogRender())lunar::diagnosticLog("render","present submit width=%d height=%d",s->frame_w,s->frame_h);
}

bool VideoRenderer::prepareDecoderReset(){
  if(video_backend_==VideoBackend::Software)return true;
  auto* s=static_cast<Deko3DRenderContext*>(ctx_);
  if(!s)return false;
  std::unique_lock<std::mutex> lock(s->render_mutex);
  if(!s->ok||!s->q)return false;
  bool has_submitted_frames=false;
  for(auto* frame:s->submitted_frames){
    if(frame){has_submitted_frames=true;break;}
  }
  if(!s->pending_frame&&!s->current_frame&&!has_submitted_frames&&
     !s->present_slice_active){
    return true;
  }
  s->decoder_reset_ready=false;
  s->decoder_reset_drain_steps=0;
  s->resolution_transition_drain_steps=0;
  s->decoder_reset_requested=true;
  constexpr auto kResetHandoffTimeout=std::chrono::milliseconds(250);
  const bool completed=s->decoder_reset_cv.wait_for(
      lock,kResetHandoffTimeout,[s](){return !s->decoder_reset_requested;});
  if(!completed){
    s->decoder_reset_requested=false;
    s->decoder_reset_drain_steps=0;
    lunar::dropDiagnosticLog("video-reset", "phase=gpu-quiesce-timeout");
    return false;
  }
  return s->decoder_reset_ready;
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
  std::lock_guard<std::mutex> render_lock(s->render_mutex);
  s->decoder_reset_requested=false;
  s->decoder_reset_ready=false;
  s->decoder_reset_drain_steps=0;
  s->resolution_transition_drain_steps=0;
  s->decoder_reset_cv.notify_all();
  if(!s->ok && !s->vctx && !s->pc && !s->pd && !s->pi)return;
  if(s->q)s->q.waitIdle();
  s->present_slice_active=false;
  s->fms.clear();
  releaseRetainedFrames(*s);
  s->resolution_transition.reset();
  destroyPostProcessTargets(*s);
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
  s->update_ring.reset();
  s->present_ring.reset();
  s->pd.reset();
  s->pc.reset();
  s->pi.reset();
  s->vctx=nullptr;
  s->dev=nullptr;
  s->q=nullptr;
  s->mapped_luma_w=0;
  s->mapped_luma_h=0;
  s->present_frame_id=0;
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
bool VideoRenderer::prepareDecoderReset(){return true;}
bool VideoRenderer::pollEvents(){SDL_Event e;while(SDL_PollEvent(&e))if(e.type==SDL_QUIT||(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE))return false;return true;}
void VideoRenderer::shutdown(){
  std::lock_guard<std::mutex> lock(sdl_mutex_);
  if(texture_){SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));texture_=nullptr;}
  if(renderer_){SDL_DestroyRenderer(static_cast<SDL_Renderer*>(renderer_));renderer_=nullptr;}
  if(window_){SDL_DestroyWindow(static_cast<SDL_Window*>(window_));window_=nullptr;}
}
}
#endif
