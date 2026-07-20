# Switch EASU/RCAS Post-Process Design

## Goal

Finish the Switch rendering backend alignment with Moonlight-Switch by adding a default-off EASU/RCAS post-process path behind a session-scoped UI switch.

## Scope

In scope:
- Replace the boolean post-process option with a three-state mode:
  - `Off`
  - `Upscale`
  - `UpscaleRcas`
- Keep the direct renderer path as the default and fallback path.
- Add Switch-only EASU and RCAS shader sources and RomFS build rules.
- Add Deko3D uniform buffers for EASU and RCAS constants.
- Record pass chains that match Moonlight-Switch's media backend shape:
  - `Off`: `NVTEGRA -> framebuffer`
  - `Upscale`: `NVTEGRA -> source target -> EASU target -> framebuffer`
  - `UpscaleRcas`: `NVTEGRA -> source target -> EASU target -> RCAS target -> framebuffer`
- Expose the mode in the Switch UI and keep the default `Off`.

Out of scope:
- Xbox protocol/session/provider changes.
- Dithering.
- Persisting the UI setting.
- Hardware-only visual tuning beyond build and static validation.

## Architecture

LunarNX keeps its existing ownership:

`StreamController -> MediaPipeline -> VideoRenderer`

The option contract becomes:

```cpp
enum class PostProcessMode { Off, Upscale, UpscaleRcas };

struct MediaPipelineOptions {
    PostProcessMode post_process_mode = PostProcessMode::Off;
};
```

`VideoRenderer` exposes `setPostProcessMode(PostProcessMode mode)`. The old `setPostProcessEnabled(bool)` remains as a compatibility wrapper and maps `true` to `Upscale`.

The Switch renderer uses the existing `RenderTarget` abstraction:

- `source_target`: frame-sized RGBA target populated by the NV12 shader with full-frame UVs.
- `upscale_target`: screen-sized RGBA target populated by the EASU shader.
- `rcas_target`: screen-sized RGBA target populated by the RCAS shader when requested.

All target descriptors are updated through `SwitchVideoContext::updateImageDescriptor()` and `CCmdMemRing<brls::FRAMEBUFFERS_COUNT>`.

## Pass Behavior

### Off

The default path stays unchanged:

`NVTEGRA luma/chroma -> texture_fsh -> framebuffer`

No post-process render targets are required.

### Upscale

The renderer allocates:

- `source_target` at stream frame size.
- `upscale_target` at Borealis window size.

It records:

1. Render NV12 to `source_target`.
2. Barrier image writes.
3. Bind `upscaling_fsh.dksh`, EASU constants, and `source_target`.
4. Render to `upscale_target`.
5. Barrier image writes.
6. Blit `upscale_target` to the framebuffer.

### UpscaleRcas

The renderer additionally allocates `rcas_target` at Borealis window size and records:

1. The same NV12 and EASU passes as `Upscale`.
2. Bind `rcas_fsh.dksh`, RCAS constants, and `upscale_target`.
3. Render to `rcas_target`.
4. Barrier image writes.
5. Blit `rcas_target` to the framebuffer.

## Constants

EASU uses a std140-compatible buffer:

```cpp
struct EasuConstants {
    alignas(16) uint32_t con0[4];
    alignas(16) uint32_t con1[4];
    alignas(16) uint32_t con2[4];
    alignas(16) uint32_t con3[4];
};
```

RCAS uses:

```cpp
struct RcasConstants {
    alignas(16) uint32_t control[4];
};
```

The initial RCAS strength is `0.2f`, matching Moonlight-Switch's conservative default.

## Error Handling

- If shaders fail to load, the renderer logs the unavailable path and falls back to direct rendering.
- If a target or descriptor update fails, the renderer releases post-process targets and falls back to direct rendering for that frame.
- If RCAS target allocation fails, `UpscaleRcas` falls back to `Upscale` when EASU resources are valid.
- Shutdown waits for the queue to idle before releasing GPU resources.
- Image slots are allocated once during initialization and freed once during shutdown.

## Testing

Required validation:

```bash
bash scripts/check_stream_regressions.sh
cmake --build build/pc -j4
make -f Makefile.desktop -j4
make -f Makefile.switch -j4
git diff --check
```

Manual Switch validation remains required before declaring hardware-perfect rendering:

- Start stream with post-process `Off`.
- Start stream with `Upscale`.
- Start stream with `Upscale+RCAS`.
- Stop/restart streams across all modes.
- Confirm no black screen, stretching, or obvious frame pacing regression.

## Acceptance Criteria

- Default UI mode is `Post Process: Off`.
- The UI can cycle `Off -> Upscale -> Upscale+RCAS -> Off`.
- PC/desktop builds still compile with no-op post-process behavior.
- Switch build generates `upscaling_fsh.dksh`, `rcas_fsh.dksh`, and `upscaling_pass_fsh.dksh`.
- Regression checks require the EASU/RCAS modes, shaders, uniforms, and pass functions.
- No Xbox/WebRTC protocol behavior changes are included.
