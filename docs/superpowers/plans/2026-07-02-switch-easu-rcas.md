# Switch EASU/RCAS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a default-off Switch EASU/RCAS rendering path that aligns LunarNX's media backend with Moonlight-Switch and remains easy to disable from UI.

**Architecture:** Keep LunarNX's current stream ownership. Propagate a `PostProcessMode` from `MainActivity` to `StreamController`, `MediaPipeline`, and `VideoRenderer`; on Switch, record Deko3D passes for direct, EASU-only, and EASU+RCAS modes with direct fallback.

**Tech Stack:** C++20, Borealis UI, Deko3D, FFmpeg NVTEGRA, uam shader compiler, shell regression checks.

## Global Constraints

- Default mode must remain `Off`.
- Xbox/WebRTC protocol code must not change for this feature.
- Deko3D descriptors must be updated through `SwitchVideoContext::updateImageDescriptor()`.
- `romfs/shaders/*.dksh` remain generated outputs.
- Required verification: `bash scripts/check_stream_regressions.sh`, `cmake --build build/pc -j4`, `make -f Makefile.desktop -j4`, `make -f Makefile.switch -j4`, `git diff --check`.

---

### Task 1: Regression Contract

**Files:**
- Modify: `scripts/check_stream_regressions.sh`

**Interfaces:**
- Produces checks for `PostProcessMode`, UI mode labels, EASU/RCAS shaders, uniforms, and render pass functions.

- [ ] **Step 1: Add failing checks**

Add checks requiring:
- `PostProcessMode`
- `post_process_mode`
- `setPostProcessMode`
- UI labels `Post Process: Off`, `Post Process: Upscale`, `Post Process: Upscale+RCAS`
- `shaders/upscaling_fsh.glsl`
- `shaders/rcas_fsh.glsl`
- Makefile/script references to `upscaling_fsh.dksh` and `rcas_fsh.dksh`
- `EasuConstants`
- `RcasConstants`
- `populateEasuConstants`
- `populateRcasConstants`
- `recordUpscalePass`
- `recordRcasPass`
- `bindUniformBuffer`

- [ ] **Step 2: Verify RED**

Run: `bash scripts/check_stream_regressions.sh`

Expected: fails because the current implementation only has a boolean post-process path.

### Task 2: Mode API and UI

**Files:**
- Modify: `src/stream/media_pipeline.h`
- Modify: `src/stream/media_pipeline.cpp`
- Modify: `src/stream/video_renderer.h`
- Modify: `src/stream/video_renderer.cpp`
- Modify: `src/ui/main_activity.h`
- Modify: `src/ui/main_activity.cpp`

**Interfaces:**
- Produces: `enum class PostProcessMode { Off, Upscale, UpscaleRcas }`
- Produces: `VideoRenderer::setPostProcessMode(PostProcessMode mode)`
- Keeps: `VideoRenderer::setPostProcessEnabled(bool enabled)` compatibility wrapper

- [ ] **Step 1: Add `PostProcessMode`**

Define the enum in `media_pipeline.h` next to `MediaPipelineOptions`.

- [ ] **Step 2: Propagate mode**

Call `video_renderer_->setPostProcessMode(options.post_process_mode)` in `MediaPipeline::initialize()`.

- [ ] **Step 3: Update Switch UI**

Replace the boolean field with `PostProcessMode post_process_mode_ = PostProcessMode::Off`, cycle the button through all three labels, and pass the mode into `MediaPipelineOptions`.

### Task 3: Shader Assets

**Files:**
- Add: `shaders/upscaling_fsh.glsl`
- Add: `shaders/rcas_fsh.glsl`
- Modify: `scripts/compile_shaders.sh`
- Modify: `Makefile.switch`

**Interfaces:**
- Produces generated RomFS assets:
  - `romfs/shaders/upscaling_fsh.dksh`
  - `romfs/shaders/rcas_fsh.dksh`

- [ ] **Step 1: Add shader sources**

Add EASU and RCAS fragment shaders based on the Moonlight-Switch algorithms.

- [ ] **Step 2: Update shader script**

Compile both new fragment shaders in `scripts/compile_shaders.sh`.

- [ ] **Step 3: Update Switch Makefile**

Add both `.dksh` targets to `ROMFS_SHADERS` and define `uam -s frag` rules.

### Task 4: Deko3D Passes

**Files:**
- Modify: `src/stream/video_renderer.cpp`

**Interfaces:**
- Produces: `EasuConstants`
- Produces: `RcasConstants`
- Produces: `populateEasuConstants(EasuConstants&, const SourceViewport&, int, int, int, int)`
- Produces: `populateRcasConstants(RcasConstants&, float)`
- Produces: `recordUpscalePass(Deko3DRenderContext&)`
- Produces: `recordRcasPass(Deko3DRenderContext&)`

- [ ] **Step 1: Add shader and uniform handles**

Load `upscaling_fsh.dksh` and `rcas_fsh.dksh`, allocate EASU/RCAS uniform buffers, and initialize RCAS strength to `0.2f`.

- [ ] **Step 2: Allocate mode-specific targets**

Update target allocation so `Upscale` requires source/upscale targets and `UpscaleRcas` additionally tries to allocate RCAS target.

- [ ] **Step 3: Record pass chain**

Record direct, EASU-only, and EASU+RCAS chains with barriers after render-to-texture passes and final blit from the correct texture slot.

- [ ] **Step 4: Fallback behavior**

If the requested mode cannot be satisfied, release post-process resources and render direct for that frame.

### Task 5: Verification and Commit

**Files:**
- All modified files.

**Interfaces:**
- Produces a passing and committed implementation.

- [ ] **Step 1: Run verification**

Run:

```bash
bash scripts/check_stream_regressions.sh
cmake --build build/pc -j4
make -f Makefile.desktop -j4
make -f Makefile.switch -j4
git diff --check
```

- [ ] **Step 2: Commit**

Commit with:

```bash
git add docs/superpowers/specs/2026-07-02-switch-easu-rcas-design.md docs/superpowers/plans/2026-07-02-switch-easu-rcas.md scripts/check_stream_regressions.sh scripts/compile_shaders.sh Makefile.switch shaders/upscaling_fsh.glsl shaders/rcas_fsh.glsl src/stream/media_pipeline.h src/stream/media_pipeline.cpp src/stream/video_renderer.h src/stream/video_renderer.cpp src/ui/main_activity.h src/ui/main_activity.cpp
git commit -m "feat: add switch upscaling and rcas passes"
```
