# Full Hardware Zero-Copy Video Design

## Goal

Add a selectable full hardware video path that decodes Xbox Remote Play H.264
with Switch NVDEC and renders the resulting NVTEGRA surfaces directly with
deko3d. The full hardware path must display a correct moving mock stream in
Ryubing. The existing NVDEC copy-out and software decode paths remain available
to the user.

The default mode is full hardware. Runtime failures do not automatically fall
back to another mode.

## Video Modes

`VideoBackend` has three explicit values:

| Mode | Decode | Frame transport | Render |
| --- | --- | --- | --- |
| `HardwareZeroCopy` | FFmpeg NVDEC | NVTEGRA/NvMap surface | deko3d NV12 shader |
| `HardwareCopyOut` | FFmpeg NVDEC | `av_hwframe_transfer_data` to CPU | existing RGBA/nanovg sink |
| `Software` | FFmpeg software H.264 | CPU frame | existing RGBA/nanovg sink |

The main activity cycles through full hardware, hardware copy-out, and software
before a stream starts. Logs and the stream performance overlay show the active
mode.

The configuration values are:

- `hardware_zero_copy`
- `hardware_copy_out`
- `software`

The legacy value `hardware` maps to `HardwareCopyOut`, preserving the behavior
of configurations created before this change. `config/default_config.json`
uses `hardware_zero_copy`.

## Full Hardware Data Flow

```text
WebRTC H.264 access unit
  -> FFmpeg NVDEC
  -> AV_PIX_FMT_NVTEGRA AVFrame
  -> AVNVTegraMap / NvMap backing memory
  -> deko3d external DkMemBlock
  -> R8 + RG8 NV12 textures
  -> NV12-to-RGB shader
  -> Borealis framebuffer
```

The NVDEC packet submission and receive loop remains shared by both hardware
modes. Only frame delivery differs:

- `HardwareZeroCopy` sends the original NVTEGRA frame to the renderer.
- `HardwareCopyOut` transfers the NVTEGRA frame to CPU memory before invoking
  the renderer.

The zero-copy mapping follows Moonlight-Switch:

- Obtain the framebuffer map with `av_nvtegra_frame_get_fbuf_map`.
- Wrap its CPU address in an external-storage `DkMemBlock` using
  `DkMemBlockFlags_Image`.
- Create an `R8_Unorm` luma image at offset zero.
- Create an `RG8_Unorm` chroma image at the `data[1] - data[0]` offset.
- Include `DkImageFlags_UsageVideo` in both image layouts.
- Update Borealis image descriptor slots and invalidate descriptors before
  submitting the draw.

Mappings are keyed by map handle, address, size, chroma offset, width, and
height. A size change waits for the GPU, clears mappings, rebuilds layouts, and
then accepts the new frame.

## Frame Ownership And GPU Synchronization

The decoder callback lends its `AVFrame` only for the duration of the callback.
The zero-copy renderer therefore takes its own `av_frame_ref` before returning
from `render()`.

The renderer uses a bounded set of in-flight slots. Each slot owns:

- One retained `AVFrame` reference.
- One `DkFence` signaled after commands that sample that frame complete.
- The state needed to distinguish an unused slot from a submitted slot.

When submitting a frame:

1. The renderer selects the next in-flight slot.
2. If the slot is still occupied, it waits for that slot's fence.
3. It releases the old frame reference only after the fence completes.
4. It records the current draw and signals the slot's fence at the end.
5. It moves the current retained frame into that slot.

This prevents FFmpeg from returning an NvMap surface to the NVDEC pool while
the GPU is still sampling it. It also replaces the previous assumption that
retaining the most recent four frames is always sufficient.

Shutdown, frame-size changes, and mapping eviction wait for outstanding GPU
work before releasing frame references or external memory mappings.

## Error Handling

There is no automatic mode fallback.

- Decoder or renderer initialization failure aborts the current stream and is
  surfaced through the existing stream error state.
- Missing NVTEGRA maps, invalid map sizes or offsets, DkMemBlock failures,
  descriptor update failures, and render submission failures are logged with
  bounded diagnostic output.
- A runtime render failure does not change the selected mode. The user can exit
  the stream and select hardware copy-out or software mode.
- App startup remains independent of media initialization. A full hardware
  failure cannot prevent LunarNX from reaching the main activity.

## Ryubing Diagnosis

Ryubing support for NvMap external storage is treated as an open question, not
as a known limitation. The implementation must be tested before assigning a
failure to the emulator.

If the output is black, diagnosis proceeds through these boundaries:

1. Confirm NVDEC emits valid `AV_PIX_FMT_NVTEGRA` frames.
2. Confirm the same frame can produce correct CPU pixels through a temporary
   diagnostic transfer.
3. Validate NvMap handle, address, size, plane offsets, pitches, and image layout
   requirements.
4. Confirm external DkMemBlock and image creation succeeds.
5. Confirm descriptor writes and invalidation succeed.
6. Confirm the NV12 shader and framebuffer draw are submitted and fenced.
7. Capture the Ryubing output and inspect actual pixels rather than relying only
   on success logs.

Temporary transfer probes must not remain enabled in the production full
hardware path.

## Verification

### Automated Tests

Add focused regression tests for:

- Three backend enum values and configuration parsing.
- UI cycle order and labels.
- Decoder/renderer routing for all three modes.
- Absence of `av_hwframe_transfer_data` from zero-copy frame delivery.
- Presence of per-slot frame references and fences.
- Legacy `hardware` configuration compatibility.

Run all existing WebRTC, stream, backend, and hardware decode regressions. Run
desktop stream and auth tests to catch shared-code regressions.

### Switch Build

Perform a clean Switch build in
`devkitpro/devkita64:20251117`. No Switch library or NRO is built with macOS
libraries.

### Ryubing Mock Stream

The first full hardware target is the existing local mock Xbox stream at
720p60. Completion requires all of the following:

- The screen shows a correct, continuously moving color video.
- A captured screenshot is nonblack and has no persistent severe corruption,
  plane offset error, or historical-frame smearing.
- Logs show NVTEGRA frames, valid NvMap mappings, descriptor updates, and GPU
  submissions.
- `decode_errors`, `missing`, `srtp_fail`, `h264_corrupt`, and
  `rtp_queue_drop` remain zero during a stable run.
- The overlay identifies the full hardware mode.

After 720p60 succeeds, run 1080p30 and use 1080p60 as a performance stress
test. Re-run a mock stream in `HardwareCopyOut` and `Software` modes to verify
that both retained paths still display correctly.

## Residual Hardware Validation

Ryubing is the required development target for this change, but it is not final
proof of Switch behavior. After simulator completion, all three modes still
require real Switch validation, with special attention to long-running NvMap
lifetime, GPU synchronization, thermals, and 1080p performance.
