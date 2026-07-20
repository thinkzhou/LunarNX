# Switch Media Parity Design

## Goal

Close the remaining Moonlight-Switch media backend gaps that are practical in LunarNX's current architecture: color handling, dithering, render/post-process stats, and Audren queue latency tuning.

## Scope

In scope:
- Dynamic YUV color matrix and offset selection for BT.601, BT.709, BT.2020, limited range, and full range.
- Switch NVTEGRA safety behavior matching Moonlight-Switch: treat NVTEGRA frames reporting JPEG range as limited range.
- Default-off dithering on the final post-process blit.
- Session-scoped UI toggle for dithering.
- Render, post-process, dithering, upscaling, sharpening, and audio latency statistics in `PerfStats`.
- R3 performance overlay display for the new stats.
- Audren queue sizing aligned with Moonlight-Switch's `AUDREN_SAMPLES_PER_FRAME_48KHZ * 5` wavebuf sizing and five wavebuf queue.
- Structural regression checks for these behaviors.

Out of scope:
- Xbox/Greenlight/XStreaming protocol changes.
- Persistent settings storage.
- Moonlight-Switch's full settings tab and in-game overlay architecture.
- Hardware-only measurement beyond building the Switch NRO.

## Architecture

LunarNX keeps:

`StreamController -> MediaPipeline -> VideoRenderer / AudioPlayer -> PerfStats -> PerfOverlay`

`MediaPipelineOptions` gains:

```cpp
bool dithering_enabled = false;
float dithering_strength = 3.0f;
```

`VideoRenderer` gains:

```cpp
void setPerfStats(PerfStats* stats);
void setDitheringEnabled(bool enabled, float strength = 3.0f);
```

The Switch renderer continues to use the existing Deko3D target pipeline. The final framebuffer pass always uses `upscaling_pass_fsh.glsl` when any post-processing is active. That shader gets a std140 `PostProcessSettings` uniform:

```glsl
vec4 control; // x = dithering enabled, y = dithering strength
```

## Rendering Paths

### Off

With post-process mode off and dithering off:

`NVTEGRA -> framebuffer`

### Dithering Only

With post-process mode off and dithering on:

`NVTEGRA -> screen-sized RGBA target -> dithering blit -> framebuffer`

### Upscale

With upscaling enabled:

`NVTEGRA -> source target -> EASU target -> optional dithering blit -> framebuffer`

### Upscale+RCAS

With RCAS enabled:

`NVTEGRA -> source target -> EASU target -> RCAS target -> optional dithering blit -> framebuffer`

## Color Handling

The renderer reads `AVFrame::colorspace` and `AVFrame::color_range`.

Supported matrices:
- BT.601 limited/full
- BT.709 limited/full
- BT.2020 limited/full

Defaults:
- Unknown colorspace falls back to BT.601.
- Unknown range falls back to limited.
- NVTEGRA frames that report `AVCOL_RANGE_JPEG` are treated as limited to avoid lifted blacks.

## Statistics

`PerfStats` records:
- Average render submit time.
- Average post-process time.
- Average dithering time.
- Average EASU upscaling time.
- Average RCAS sharpening time.
- Post-processed/dithered/upscaled/sharpened frame counters.
- Estimated queued audio latency in milliseconds.
- Audio buffer duration and overflow threshold in milliseconds.

These stats are best-effort CPU-side measurements. They are useful for regression and tuning, not a replacement for true GPU timestamp queries or Switch hardware profiling.

## Audio Queue Tuning

Audren keeps five wavebufs. Each wavebuf uses:

```cpp
AUDREN_SAMPLES_PER_FRAME_48KHZ * 5
```

At 48 kHz this matches Moonlight-Switch's low-latency Audren sizing more closely than the previous `sample_rate / 50 * 5` sizing.

The player estimates queued audio latency from:

```cpp
queued_samples = total_queued_samples - audrvVoiceGetPlayedSampleCount(...)
latency_ms = queued_samples * 1000 / sample_rate
```

Overflow protection remains conservative and drops incoming audio if estimated queued samples exceed the configured overflow window.

## Error Handling

- If dithering is enabled but post-process resources fail, the renderer falls back to direct rendering.
- If EASU/RCAS resources fail, existing downgrade behavior remains.
- Dithering is always default off and session-scoped for troubleshooting.
- Perf stats reset at stream start.

## Testing

Required:

```bash
bash scripts/check_stream_regressions.sh
cmake --build build/pc -j4
make -f Makefile.desktop -j4
make -f Makefile.switch -j4
git diff --check
```

Manual Switch validation still required:
- Off mode.
- Dithering-only mode.
- Upscale.
- Upscale+RCAS.
- Verify overlay stats update under R3.
- Verify audio latency estimate remains bounded and audio does not drift badly.
