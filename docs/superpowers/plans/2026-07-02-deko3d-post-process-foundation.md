# Deko3D Post-Process Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a default-off Switch UI toggle that enables a Moonlight-Switch-style Deko3D post-process foundation path.

**Architecture:** Keep LunarNX's Xbox/WebRTC session layer unchanged. Pass a session-scoped `MediaPipelineOptions` object from UI to `StreamController`, then into `MediaPipeline` and `VideoRenderer`. On Switch, `VideoRenderer` owns optional RGBA render targets and can render `NV12 -> source target -> framebuffer`; with the option off it keeps the current direct path.

**Tech Stack:** C++17/20, Borealis UI, Deko3D, libnx, FFmpeg NVTEGRA, shell regression checks, uam shader compiler.

## Global Constraints

- Default behavior must remain direct `NV12 -> framebuffer`.
- The post-process path must default off and be toggleable before starting a stream.
- Do not change Xbox/WebRTC protocol behavior.
- Do not copy Moonlight-Switch's full `DKVideoRenderer` class.
- Deko3D descriptors must be updated through Borealis image slots and `updateImageDescriptor()`.
- Validation must run: `bash scripts/check_stream_regressions.sh`, `cmake --build build/pc -j4`, `make -f Makefile.desktop -j4`, `make -f Makefile.switch -j4`.

---

### Task 1: Regression Checks

**Files:**
- Modify: `scripts/check_stream_regressions.sh`

**Interfaces:**
- Produces structural checks that fail before implementation and pass after implementation.

- [ ] **Step 1: Add failing regression checks**

Add checks for:
- `MediaPipelineOptions`
- `post_process_enabled`
- `setPostProcessEnabled`
- `RenderTarget`
- `ensurePostProcessTargets`
- `releasePostProcessTargets`
- `upscaling_pass_fsh.dksh`
- `post_process_btn_`

- [ ] **Step 2: Verify RED**

Run: `bash scripts/check_stream_regressions.sh`

Expected: fails because the symbols and shader do not exist yet.

### Task 2: Option Propagation and UI Toggle

**Files:**
- Modify: `src/stream/media_pipeline.h`
- Modify: `src/stream/media_pipeline.cpp`
- Modify: `src/stream/video_renderer.h`
- Modify: `src/app/stream_controller.h`
- Modify: `src/app/stream_controller.cpp`
- Modify: `src/ui/main_activity.h`
- Modify: `src/ui/main_activity.cpp`

**Interfaces:**
- Produces: `stream::MediaPipelineOptions { bool post_process_enabled = false; }`
- Produces: `VideoRenderer::setPostProcessEnabled(bool enabled)`
- Changes: `StreamController::startStream(..., const stream::MediaPipelineOptions& options = {})`

- [ ] **Step 1: Add options types and renderer setter**

Add a session option object in `media_pipeline.h`, pass it into `MediaPipeline::initialize()`, and expose `VideoRenderer::setPostProcessEnabled()`.

- [ ] **Step 2: Pass options through StreamController**

Update `StreamController::startStream()` to accept options and pass them to `media_->initialize()`.

- [ ] **Step 3: Add Switch UI toggle**

Add a `post_process_btn_` button to `MainActivity`, default `post_process_enabled_ = false`, and pass the selected value into `startStream()`.

- [ ] **Step 4: Build check**

Run: `cmake --build build/pc -j4`

Expected: desktop build passes.

### Task 3: Shader Asset

**Files:**
- Add: `shaders/upscaling_pass_fsh.glsl`
- Generate: `romfs/shaders/upscaling_pass_fsh.dksh`
- Modify: `scripts/compile_shaders.sh`
- Modify: `Makefile.switch`

**Interfaces:**
- Produces: a Deko3D fragment shader that samples one RGBA texture and writes it to the framebuffer.

- [ ] **Step 1: Add blit shader source**

Add a minimal `upscaling_pass_fsh.glsl` based on Moonlight-Switch's final post-process pass, with sampler binding 0 and no visible effect when dithering is disabled.

- [ ] **Step 2: Update shader compile script**

Make `scripts/compile_shaders.sh` compile all three shaders into `romfs/shaders/*.dksh`.

- [ ] **Step 3: Add Switch Makefile shader rule**

Make `Makefile.switch` generate `romfs/shaders/upscaling_pass_fsh.dksh` before packaging the NRO.

- [ ] **Step 4: Compile shader**

Run: `DEVKITPRO=/opt/devkitpro scripts/compile_shaders.sh`

Expected: `romfs/shaders/upscaling_pass_fsh.dksh` exists.

### Task 4: Deko3D Post-Process Foundation

**Files:**
- Modify: `src/stream/video_renderer.cpp`
- Modify: `src/stream/video_renderer.h`

**Interfaces:**
- Consumes: `VideoRenderer::setPostProcessEnabled(bool enabled)`
- Produces: Switch-only `RenderTarget`, `ensurePostProcessTargets()`, `releasePostProcessTargets()`, and alternate present path.

- [ ] **Step 1: Add render target ownership**

Add `RenderTarget` for source/upscale/rcas metadata and an image pool to `Deko3DRenderContext`.

- [ ] **Step 2: Allocate and update descriptors**

Implement `ensurePostProcessTargets()` and `releasePostProcessTargets()` using `DkImageFormat_RGBA8_Unorm`, `DkImageFlags_UsageRender | DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine`, and `SwitchVideoContext::updateImageDescriptor()`.

- [ ] **Step 3: Record post-process pass**

When enabled and source target allocation succeeds:
- render NV12 to source RGBA target,
- bind source texture,
- draw the post-process blit shader to the Borealis framebuffer.

If allocation or shader load fails, fall back to direct rendering.

- [ ] **Step 4: Release resources safely**

Free source/upscale/rcas image slots and release render target memory in `shutdown()`.

### Task 5: Verification and Commit

**Files:**
- All modified implementation files.

**Interfaces:**
- Produces a clean, committed implementation.

- [ ] **Step 1: Run full verification**

Run:

```bash
bash scripts/check_stream_regressions.sh
cmake --build build/pc -j4
make -f Makefile.desktop -j4
make -f Makefile.switch -j4
```

Expected: all commands exit 0 and Switch NRO is produced.

- [ ] **Step 2: Commit**

Commit implementation with:

```bash
git add docs/superpowers/specs/2026-07-02-deko3d-post-process-foundation-design.md docs/superpowers/plans/2026-07-02-deko3d-post-process-foundation.md scripts/check_stream_regressions.sh scripts/compile_shaders.sh Makefile.switch shaders/upscaling_pass_fsh.glsl src/stream/media_pipeline.h src/stream/media_pipeline.cpp src/stream/video_renderer.h src/stream/video_renderer.cpp src/app/stream_controller.h src/app/stream_controller.cpp src/ui/main_activity.h src/ui/main_activity.cpp
git commit -m "feat: add switch post process render path"
```
