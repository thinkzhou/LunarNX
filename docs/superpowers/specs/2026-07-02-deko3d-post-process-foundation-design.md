# Deko3D Post-Process Foundation Design

## Goal

Build the Switch renderer foundation needed to align LunarNX's media backend with Moonlight-Switch and support future upscaling/RCAS work, without changing default visual output in this phase.

## Scope

This phase is limited to the Deko3D renderer internals behind `src/stream/video_renderer.cpp`.

In scope:
- Add Switch-only render target ownership for post-processing.
- Add source/upscale/rcas target metadata, descriptors, and image descriptor slots.
- Add resize/release/update logic modeled after Moonlight-Switch's Deko3D renderer.
- Add a safe pass-selection path that defaults to direct `NV12 -> framebuffer`.
- Add a user-visible Switch UI toggle for the post-process path. It defaults to off.
- Extend regression checks so these renderer resources do not regress.

Out of scope:
- Enabling EASU/upscaling.
- Enabling RCAS sharpening.
- Replacing LunarNX's `MediaPipeline` or Xbox/WebRTC session flow.
- Copying Moonlight-Switch's full `DKVideoRenderer` class.

## Architecture

LunarNX keeps its current high-level shape:

`StreamController -> MediaPipeline -> VideoDecoder -> VideoRenderer`

The Switch renderer remains the platform-specific implementation. Internally, it gains a Moonlight-Switch-style post-process resource layer:

- `FrameMapping`: maps NVTEGRA decode frames to luma/chroma Deko3D images.
- `RenderTarget`: owns an RGBA Deko3D image, memory handle, layout, descriptor, dimensions, and image slot.
- `RenderPipelineConfig`: describes which passes are active.
- `ensurePostProcessTargets()`: allocates or refreshes render targets when frame/screen size or pass usage changes.
- `releasePostProcessTargets()`: tears down GPU image resources on shutdown or resize.

The initial pipeline remains:

`NVTEGRA frame -> luma/chroma descriptors -> texture shader -> Borealis framebuffer`

When the post-process toggle is enabled, this phase supports:

`NVTEGRA frame -> source RGBA target -> post-process blit shader -> Borealis framebuffer`

Future phases can change the active pipeline to:

`NVTEGRA frame -> source RGBA target -> EASU target -> optional RCAS target -> framebuffer`

## Component Design

### VideoRenderer Switch Context

Extend `Deko3DRenderContext` with:

- A GPU image memory pool for post-process targets.
- Three optional `RenderTarget` instances:
  - `source_target`: frame-sized RGBA target.
  - `upscale_target`: screen-sized RGBA target.
  - `rcas_target`: screen-sized RGBA target.
- Image slot IDs for source/upscale/rcas descriptors.
- Target size cache for frame and screen dimensions.
- Boolean capability/request flags for post-process passes.

The direct path must work even if post-process allocation fails.

### UI Toggle

`MainActivity` adds a post-process button near the resolution controls:

- Default state: off.
- Off: `StreamController::startStream()` uses the direct renderer path.
- On: `StreamController::startStream()` enables the renderer post-process path for that session.
- The setting is session-scoped and intentionally not persisted in this phase.

The UI text must make it clear whether the feature is on or off without requiring the stream to start.

### RenderTarget

`RenderTarget` owns exactly one Deko3D RGBA image:

- `CMemPool::Handle handle`
- `dk::ImageLayout layout`
- `dk::Image image`
- `dk::ImageDescriptor descriptor`
- `int texture_slot`
- `int width`
- `int height`

It must expose a simple `bool allocated() const` check. Releasing a target destroys the handle and resets image/layout/descriptor/dimensions without touching unrelated image slots.

### Descriptor Updates

Descriptor updates must continue to use:

- `brls::SwitchVideoContext::updateImageDescriptor()`
- `brls::SwitchVideoContext::invalidateImageDescriptors()`
- `CCmdMemRing<brls::FRAMEBUFFERS_COUNT>`

The renderer must not write Deko3D descriptor memory directly.

### Pass Selection

This phase keeps:

- `post_process_enabled = false` by default.
- `upscaling_enabled = false`.
- `rcas_enabled = false`

The pass configuration remains explicit so a later settings layer can toggle it without changing the renderer lifecycle.

## Data Flow

1. `VideoDecoder` emits an NVTEGRA `AVFrame`.
2. `MediaPipeline` calls `VideoRenderer::render(frame)`.
3. `VideoRenderer::render()` maps or reuses the NVTEGRA frame mapping.
4. Luma/chroma descriptors are updated if the frame mapping changed.
5. `MediaPipeline` schedules `VideoRenderer::present()` on Borealis main thread.
6. `VideoRenderer::present()` records the active pass pipeline and submits Deko3D commands.
7. If post-process is off, output remains direct render to Borealis framebuffer.
8. If post-process is on, output renders to a source RGBA target and then blits to the Borealis framebuffer.

## Error Handling

- If luma/chroma descriptor update fails, `render()` returns `false` and no stale descriptor should be presented.
- If post-process target allocation fails while post-process is disabled, direct rendering continues.
- If post-process is enabled and allocation fails, renderer falls back to direct rendering and logs the failure.
- On shutdown, queue idle wait happens before GPU resource release.
- Image slots are always freed exactly once.

## Testing

Required validation commands:

```bash
bash scripts/check_stream_regressions.sh
cmake --build build/pc -j4
make -f Makefile.desktop -j4
make -f Makefile.switch -j4
```

Regression script additions:

- Verify renderer has `RenderTarget`/post-process target ownership.
- Verify renderer still uses Borealis image slot descriptor updates.
- Verify renderer still uses per-frame command rings.
- Verify no direct Deko3D descriptor memory rewrite appears.
- Verify the UI exposes a post-process toggle that defaults off.
- Verify `StreamController -> MediaPipeline -> VideoRenderer` propagates post-process options.
- Verify the post-process blit shader is included in RomFS.

Manual Switch validation after implementation:

- Start stream at 720p.
- Confirm video still presents normally.
- Start stream with post-process off and confirm video still presents normally.
- Start stream with post-process on and confirm video still presents normally.
- Confirm stop/restart stream does not crash.
- Confirm app shutdown/reopen does not leak visible GPU state or hang.

## Acceptance Criteria

- Default streaming output remains visually unchanged.
- Post-process path is disabled by default and can be enabled from the Switch UI before connecting.
- Switch build produces `build/switch/LunarNX.nro`.
- PC/desktop builds still pass.
- Regression script passes.
- The renderer has concrete render target resource structures ready for EASU/RCAS implementation.
- No Xbox protocol or WebRTC behavior changes are included in this phase.
