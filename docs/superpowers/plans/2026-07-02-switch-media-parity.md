# Switch Media Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the practical Moonlight-Switch media parity items in LunarNX: color handling, dithering, render stats, post-process stats, and Audren latency tuning.

**Architecture:** Keep LunarNX's existing stream controller and media pipeline. Extend session-scoped media options, feed them to the Switch renderer, record stats into `PerfStats`, and display them in the existing R3 performance overlay.

**Tech Stack:** C++20, Deko3D, Borealis, FFmpeg AVFrame metadata, Audren, shell regression checks.

## Global Constraints

- Default dithering must remain off.
- Existing post-process `Off / Upscale / UpscaleRcas` behavior must remain session-scoped.
- No Xbox/WebRTC protocol changes.
- Deko3D descriptors must continue using Borealis image slots.
- Generated RomFS shader binaries must remain untracked.
- Required verification: `bash scripts/check_stream_regressions.sh`, `cmake --build build/pc -j4`, `make -f Makefile.desktop -j4`, `make -f Makefile.switch -j4`, `git diff --check`.

---

### Task 1: Regression Contract

**Files:**
- Modify: `scripts/check_stream_regressions.sh`

**Interfaces:**
- Produces structural checks for color matrices, dithering, render stats, post-process stats, overlay stats, and Audren tuning constants.

- [ ] **Step 1: Add failing checks**

Require these symbols:
- `getFrameColorInfo`
- `AVCOL_SPC_BT2020_NCL`
- `bt2020`
- `DitheringConstants`
- `setDitheringEnabled`
- `dithering_enabled`
- `recordRenderSubmit`
- `recordPostProcess`
- `recordUpscaling`
- `recordSharpening`
- `recordDithering`
- `audio_latency_ms`
- `AUDREN_SAMPLES_PER_FRAME_48KHZ`
- `kAudioOverflowMs`

- [ ] **Step 2: Verify RED**

Run: `bash scripts/check_stream_regressions.sh`

Expected: fails before implementation.

### Task 2: Options and Stats

**Files:**
- Modify: `src/stream/media_pipeline.h`
- Modify: `src/stream/media_pipeline.cpp`
- Modify: `src/stream/video_renderer.h`
- Modify: `src/stream/perf_stats.h`
- Modify: `src/ui/main_activity.h`
- Modify: `src/ui/main_activity.cpp`
- Modify: `src/ui/perf_overlay.cpp`

**Interfaces:**
- Produces: `MediaPipelineOptions::dithering_enabled`
- Produces: `MediaPipelineOptions::dithering_strength`
- Produces: `VideoRenderer::setPerfStats(PerfStats*)`
- Produces: `VideoRenderer::setDitheringEnabled(bool, float)`

- [ ] **Step 1: Extend options**

Add dithering fields to `MediaPipelineOptions` and pass them through `MediaPipeline`.

- [ ] **Step 2: Extend stats**

Add atomic counters and average helper methods for render, post-process, EASU, RCAS, dithering, and audio latency stats.

- [ ] **Step 3: Update UI**

Add a default-off `Dithering: Off/On` button next to post-process settings and pass it into stream options.

- [ ] **Step 4: Update overlay**

Show render, post-process, upscaling, sharpening, dithering, and audio latency stats in `PerfOverlay`.

### Task 3: Color and Dithering Renderer

**Files:**
- Modify: `src/stream/video_renderer.cpp`
- Modify: `shaders/upscaling_pass_fsh.glsl`

**Interfaces:**
- Produces: `getFrameColorInfo(AVFrame*, AVColorSpace&, bool&)`
- Produces: `DitheringConstants`
- Produces: dynamic BT.601/709/2020 transform selection

- [ ] **Step 1: Add color matrix selection**

Use `AVFrame::colorspace` and `AVFrame::color_range` to select BT.601/709/2020 limited/full transforms.

- [ ] **Step 2: Add dithering shader settings**

Add `PostProcessSettings` uniform to the final blit shader and bind a Deko3D uniform buffer from the renderer.

- [ ] **Step 3: Add dithering-only path**

When post-process mode is off but dithering is enabled, render NV12 directly into a screen-sized target and use the final blit shader to dither into the framebuffer.

- [ ] **Step 4: Record stats**

Record render submit, post-process, upscaling, sharpening, and dithering timings into `PerfStats`.

### Task 4: Audren Latency Tuning

**Files:**
- Modify: `src/stream/audio_player.h`
- Modify: `src/stream/audio_player.cpp`
- Modify: `src/stream/perf_stats.h`

**Interfaces:**
- Produces: Audren wavebuf size based on `AUDREN_SAMPLES_PER_FRAME_48KHZ * kAudioLatencyFrames`.
- Produces: `kAudioOverflowMs`
- Produces: audio queue latency stats.

- [ ] **Step 1: Align wavebuf sizing**

Replace `sample_rate / 50` sizing with `AUDREN_SAMPLES_PER_FRAME_48KHZ`.

- [ ] **Step 2: Track queued latency**

Estimate queued audio latency from queued samples and record it in `PerfStats`.

- [ ] **Step 3: Expose tuning stats**

Record buffer duration and overflow threshold milliseconds for overlay/debugging.

### Task 5: Verification and Commit

**Files:**
- All modified files.

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
git add docs/superpowers/specs/2026-07-02-switch-media-parity-design.md docs/superpowers/plans/2026-07-02-switch-media-parity.md scripts/check_stream_regressions.sh src/stream/perf_stats.h src/stream/media_pipeline.h src/stream/media_pipeline.cpp src/stream/video_renderer.h src/stream/video_renderer.cpp src/stream/audio_player.h src/stream/audio_player.cpp src/ui/main_activity.h src/ui/main_activity.cpp src/ui/perf_overlay.cpp shaders/upscaling_pass_fsh.glsl
git commit -m "feat: improve switch media parity"
```
