# Full Hardware Zero-Copy Video Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a default, user-selectable NVDEC-to-deko3d zero-copy mode that displays a correct mock Xbox stream in Ryubing while retaining the NVDEC copy-out and software modes.

**Architecture:** Split the existing two-value `VideoBackend` into three explicit end-to-end modes. Both hardware modes share NVDEC packet submission; frame delivery either retains the NVTEGRA surface for deko3d or transfers it to CPU memory. The zero-copy renderer owns each sampled `AVFrame` until a per-slot deko3d fence confirms that GPU work has completed.

**Tech Stack:** C++17, FFmpeg 7.1 NVTEGRA/NVDEC, deko3d 0.5, Borealis, libnx, Python source-contract regressions, Docker `devkitpro/devkita64:20251117`, Ryubing Canary 1.3.333.

## Global Constraints

- Default mode is `HardwareZeroCopy`; no automatic fallback is allowed.
- Preserve `HardwareCopyOut` and `Software` as user-selectable modes.
- Legacy config value `hardware` continues to mean the previously shipped copy-out behavior.
- Build all Switch code and libraries in Docker, never against macOS libraries.
- Keep the legacy local libpeer path and current wiliwili-derived FFmpeg build.
- App startup remains independent of streaming and media initialization.
- Completion requires a visibly correct moving full-hardware mock stream in Ryubing.

---

### Task 1: Define And Select Three Video Modes

**Files:**
- Create: `tests/video_backend_modes_test.py`
- Modify: `src/stream/media_pipeline.h`
- Modify: `src/ui/auth_activity.cpp`
- Modify: `src/ui/main_activity.cpp`
- Modify: `src/ui/main_activity.h`
- Modify: `src/app/stream_controller.h`
- Modify: `config/default_config.json`

**Interfaces:**
- Produces: `enum class VideoBackend { HardwareZeroCopy, HardwareCopyOut, Software }`.
- Produces: `const char* videoBackendName(VideoBackend)` with stable config/log names.
- Consumes: `StreamController::setDefaultVideoBackend` and `getDefaultVideoBackend` without changing their signatures.

- [ ] **Step 1: Write the failing mode-contract test**

Create `tests/video_backend_modes_test.py` with assertions that require all three enum values, `HardwareZeroCopy` defaults, explicit config strings, legacy `hardware` parsing to `HardwareCopyOut`, and the UI cycle order `HardwareZeroCopy -> HardwareCopyOut -> Software -> HardwareZeroCopy`.

```python
require("HardwareZeroCopy" in media and "HardwareCopyOut" in media,
        "VideoBackend must expose zero-copy and copy-out hardware modes")
require("VideoBackend video_backend = VideoBackend::HardwareZeroCopy" in media,
        "Full hardware must be the pipeline default")
require('strcmp(value, "hardware") == 0' in auth and
        "VideoBackend::HardwareCopyOut" in auth,
        "Legacy hardware config must retain copy-out behavior")
require('"video_backend": "hardware_zero_copy"' in config,
        "Default config must select full hardware")
```

- [ ] **Step 2: Run the new test and verify it fails**

Run: `python3 tests/video_backend_modes_test.py`

Expected: FAIL because `HardwareZeroCopy` and `HardwareCopyOut` do not exist.

- [ ] **Step 3: Implement the enum, config parser, defaults, labels, and cycle**

Use these stable names in `media_pipeline.h`:

```cpp
enum class VideoBackend {
    HardwareZeroCopy,
    HardwareCopyOut,
    Software,
};

inline const char* videoBackendName(VideoBackend backend) {
    switch (backend) {
        case VideoBackend::HardwareZeroCopy: return "hardware_zero_copy";
        case VideoBackend::HardwareCopyOut: return "hardware_copy_out";
        case VideoBackend::Software: return "software";
    }
    return "unknown";
}
```

Parse `hardware_zero_copy`, `hardware_copy_out`, and `software` explicitly.
Parse legacy `hardware` as `HardwareCopyOut`. Set defaults in
`MediaPipelineOptions`, `StreamController`, and `MainActivity` to
`HardwareZeroCopy`. Display the labels `Decoder: Full Hardware`,
`Decoder: Hardware Copy`, and `Decoder: Software`.

- [ ] **Step 4: Run backend mode regressions**

Run: `python3 tests/video_backend_modes_test.py`

Expected: `Video backend mode tests passed`.

Run: `python3 tests/software_video_backend_test.py`

Expected: PASS after updating its old two-mode assertions to the new names.

- [ ] **Step 5: Commit the mode model**

```bash
git add config/default_config.json src/app/stream_controller.h \
  src/stream/media_pipeline.h src/ui/auth_activity.cpp \
  src/ui/main_activity.cpp src/ui/main_activity.h \
  tests/video_backend_modes_test.py tests/software_video_backend_test.py
git commit -m "Add three selectable video decode modes"
```

---

### Task 2: Route Zero-Copy, Copy-Out, And Software Frames

**Files:**
- Modify: `tests/hardware_decode_fallback_test.py`
- Modify: `src/stream/video_decoder.cpp`
- Modify: `src/stream/video_decoder.h`
- Modify: `src/stream/media_pipeline.cpp`
- Modify: `src/stream/video_renderer.h`
- Modify: `src/stream/video_renderer.cpp`

**Interfaces:**
- Consumes: `VideoBackend` values from Task 1.
- Produces: `bool usesHardwareDecode(VideoBackend)` and `bool usesZeroCopyRender(VideoBackend)` helpers in `media_pipeline.h`.
- Preserves: `VideoDecoder::setVideoBackend(VideoBackend)` and `VideoRenderer::setVideoBackend(VideoBackend)`.

- [ ] **Step 1: Change the hardware regression test to require exact routing**

Require both hardware modes to initialize NVDEC. Require
`av_hwframe_transfer_data` only under `HardwareCopyOut`. Require the pipeline to
pass `HardwareZeroCopy` to the renderer and to map the other two modes to the
CPU renderer.

```python
require("usesHardwareDecode(video_backend_)" in decoder,
        "Both hardware modes must share NVDEC")
require("video_backend_ == VideoBackend::HardwareCopyOut" in decoder,
        "Only copy-out mode may transfer NVDEC frames to CPU")
require("usesZeroCopyRender(options.video_backend)" in pipeline,
        "Pipeline must select deko3d only for zero-copy mode")
```

- [ ] **Step 2: Run the changed test and verify it fails**

Run: `python3 tests/hardware_decode_fallback_test.py`

Expected: FAIL on the old `VideoBackend::Hardware` routing.

- [ ] **Step 3: Implement shared NVDEC and mode-specific delivery**

Add helpers:

```cpp
inline bool usesHardwareDecode(VideoBackend backend) {
    return backend == VideoBackend::HardwareZeroCopy ||
           backend == VideoBackend::HardwareCopyOut;
}

inline bool usesZeroCopyRender(VideoBackend backend) {
    return backend == VideoBackend::HardwareZeroCopy;
}
```

Use `usesHardwareDecode` for NVDEC initialization and packet submission.
Transfer NVTEGRA frames only when the selected backend is
`HardwareCopyOut`. Keep the original NVTEGRA `AVFrame` for
`HardwareZeroCopy`. In `MediaPipeline::initialize`, pass
`HardwareZeroCopy` to `VideoRenderer` only for zero-copy mode and pass
`Software` for both CPU-frame modes.

- [ ] **Step 4: Run decoder and pipeline tests**

Run: `python3 tests/hardware_decode_fallback_test.py`

Expected: `Hardware decode fallback tests passed`.

Run: `python3 tests/video_pipeline_logging_test.py`

Expected: PASS.

Run: `python3 tests/media_pipeline_async_test.py`

Expected: PASS.

- [ ] **Step 5: Commit the routing changes**

```bash
git add src/stream/media_pipeline.h src/stream/media_pipeline.cpp \
  src/stream/video_decoder.h src/stream/video_decoder.cpp \
  src/stream/video_renderer.h src/stream/video_renderer.cpp \
  tests/hardware_decode_fallback_test.py
git commit -m "Route full hardware frames to deko3d"
```

---

### Task 3: Make Zero-Copy Frame Lifetime Fence-Safe

**Files:**
- Create: `tests/zero_copy_frame_lifetime_test.py`
- Modify: `src/stream/video_renderer.cpp`
- Modify: `docs/hw_decode_experiments.md`

**Interfaces:**
- Consumes: NVTEGRA `VideoFrame::avframe` from Task 2.
- Produces: renderer-owned `InFlightFrame` slots containing `AVFrame*`, `dk::Fence`, and `submitted`.
- Preserves: synchronous `bool VideoRenderer::render(const VideoFrame&)` and main-thread `void VideoRenderer::present()`.

- [ ] **Step 1: Write the failing ownership test**

Create `tests/zero_copy_frame_lifetime_test.py` requiring an explicit pending
frame, per-slot fences, `av_frame_ref`, `Fence::wait`, fence signaling after
draw commands, and GPU-idle cleanup before mapping destruction.

```python
require("struct InFlightFrame" in renderer and "dk::Fence fence" in renderer,
        "Zero-copy renderer needs fenced frame slots")
require("pending_frame" in renderer and "av_frame_ref" in renderer,
        "Renderer must retain the borrowed decoder frame")
require("fence.wait" in renderer and "signalFence" in renderer,
        "Frame reuse must be synchronized to GPU completion")
```

- [ ] **Step 2: Run the test and verify it fails**

Run: `python3 tests/zero_copy_frame_lifetime_test.py`

Expected: FAIL because the renderer currently has only an unfenced
`retained_frames` ring.

- [ ] **Step 3: Add pending and in-flight ownership**

Replace `retained_frames` with:

```cpp
struct InFlightFrame {
    AVFrame* frame = nullptr;
    dk::Fence fence{};
    bool submitted = false;
};

AVFrame* pending_frame = nullptr;
std::array<InFlightFrame, brls::FRAMEBUFFERS_COUNT + 1> in_flight_frames{};
size_t next_in_flight_frame = 0;
```

`render()` takes an `av_frame_ref`, releases a superseded unsubmitted pending
frame, and stores the new reference. `present()` waits for the selected slot's
old fence before releasing its frame, records the draw, signals that slot's
fence after the draw, submits the command list, and moves `pending_frame` into
the slot. `shutdown()` waits for queue idle before freeing pending/in-flight
frames and mappings.

- [ ] **Step 4: Align mapping construction and validate map geometry**

Use Moonlight-Switch's C++ external storage construction:

```cpp
mapping.memblock = dk::MemBlockMaker{s.dev, size}
    .setFlags(DkMemBlockFlags_CpuUncached |
              DkMemBlockFlags_GpuCached |
              DkMemBlockFlags_Image)
    .setStorage(cpu_addr)
    .create();
```

Before image initialization, reject zero handles/addresses/sizes, chroma
offsets outside the map, and layouts whose plane offset plus layout size exceeds
the NvMap. Log map handle, address, size, chroma offset, pitches, and layout
sizes for the first bounded set of frames.

- [ ] **Step 5: Run lifetime and renderer regressions**

Run: `python3 tests/zero_copy_frame_lifetime_test.py`

Expected: `Zero-copy frame lifetime tests passed`.

Run: `python3 tests/deko3d_post_process_foundation_test.py`

Expected: PASS.

Run: `python3 tests/deko3d_post_process_runtime_test.py`

Expected: PASS.

- [ ] **Step 6: Commit the fence-safe renderer**

```bash
git add src/stream/video_renderer.cpp tests/zero_copy_frame_lifetime_test.py \
  docs/hw_decode_experiments.md
git commit -m "Fence zero-copy NVDEC frame lifetime"
```

---

### Task 4: Expose Active Mode And Verify End To End

**Files:**
- Modify: `src/ui/stream_overlay.h`
- Modify: `src/ui/stream_overlay.cpp`
- Modify: `src/ui/stream_view.cpp`
- Modify: `docs/ryujinx_testing.md`
- Modify: `docs/hw_decode_experiments.md`

**Interfaces:**
- Consumes: `StreamController::getDefaultVideoBackend()` and `videoBackendName`.
- Produces: `StreamOverlay::update(float, const std::string&, int, const std::string&)`.

- [ ] **Step 1: Add the active mode to the top stream overlay**

Pass the selected backend name from `StreamView` into `StreamOverlay::update`
and append a compact `HW-ZC`, `HW-Copy`, or `SW` marker to the existing top
status line. Keep the overlay height fixed and verify the longest 1080p status
line does not overflow the 1280-pixel Switch viewport.

- [ ] **Step 2: Run all focused and desktop tests**

Run every `tests/*_test.py`, then:

```bash
make -f Makefile.desktop stream_tests auth_ui_tests auth_test
```

Expected: all tests and binaries complete successfully.

- [ ] **Step 3: Clean-build the Switch NRO in Docker**

Run the AGENTS.md Docker clean command, followed by:

```bash
make -f Makefile.switch -j$(nproc) \
  NETWORK_DIAG=0 CURL_PROVIDER=wiliwili CURL_VERIFY=0 \
  CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
```

Expected: `build/switch/LunarNX.nro` is produced without errors.

- [ ] **Step 4: Run the 720p60 Ryubing full-hardware mock**

Start `tools/mock_xbox/mock_xbox_server.py` with the tracked 720p60 test video,
launch `build/switch/LunarNX.nro` with `scripts/run_ryubing_nro.sh`, select
`Full Hardware`, and connect. Inspect the app log and capture the simulator
window.

Expected:

- Correct continuously moving color video.
- Overlay contains `HW-ZC`.
- NVTEGRA map and GPU submit logs are present.
- `decode_errors`, `missing`, `srtp_fail`, `h264_corrupt`, and
  `rtp_queue_drop` are zero.

- [ ] **Step 5: Diagnose any black or corrupted output by boundary**

Use a temporary copy-out of the same retained NVTEGRA frame to prove decoded
pixels, then compare map size/offset/layout logs, descriptor results, and GPU
submission fences. Remove the transfer probe after identifying the failing
boundary. Do not leave CPU transfer enabled in `HardwareZeroCopy`.

- [ ] **Step 6: Regress copy-out and software modes**

Run the same mock once with `Hardware Copy` and once with `Software`.

Expected: both modes display the moving test video and retain zero RTP/SRTP/H.264
integrity errors.

- [ ] **Step 7: Run 1080p validation and finish checks**

Run 1080p30 full hardware, then 1080p60 as a stress test. Record FPS, average
decode time, render submit time, and any frame drops in
`docs/hw_decode_experiments.md`.

Run: `git diff --check`

Expected: no output.

- [ ] **Step 8: Commit verification and documentation**

```bash
git add src/ui/stream_overlay.h src/ui/stream_overlay.cpp \
  src/ui/stream_view.cpp docs/ryujinx_testing.md docs/hw_decode_experiments.md
git commit -m "Validate full hardware streaming in Ryubing"
```
