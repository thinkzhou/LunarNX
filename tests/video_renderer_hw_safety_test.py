#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    renderer = Path("src/stream/video_renderer.cpp").read_text()
    present_start = renderer.index("void VideoRenderer::present()")
    present_end = renderer.index("void VideoRenderer::drainDecoderFrames()", present_start)
    present = renderer[present_start:present_end]

    require("s->present_ring->begin(s->present_cb)" in present,
            "hardware presentation must use the existing command-ring fence")
    require("s->q.waitIdle()" not in present,
            "present must not block before Borealis endFrame/presentImage")
    require("submitted_frames" in present and "completed_frame" in present,
            "GPU-used NVDEC frames must be retained until their command slice retires")
    require("s->next_submitted_frame=(submitted_index+1)%s->submitted_frames.size()" in present,
            "frame retirement must advance with the command-ring slice")
    require("luma_delta" in renderer and "chroma_delta" in renderer,
            "NVTEGRA plane offsets must be computed from the NvMap base")
    require("cropToMappedSurface" in renderer,
            "sampling must crop aligned NVTEGRA surfaces to visible dimensions")
    require("retireCompletedTargets(*s, submitted_index)" in present and
            "present_slice_active=true" in present,
            "post-process targets must retire against the same command-ring fence")
    require("pending_slice_mask" in renderer and
            "deferResetRenderTarget" in renderer,
            "GPU-used render targets must not be destroyed immediately")
    require("MaxRetiredTargets" in renderer and
            "retired_targets.size() >= MaxRetiredTargets" in renderer,
            "deferred target storage must remain bounded on repeated resize/toggle events")
    record_pipeline_start = renderer.index("void recordPresentPipeline(")
    record_pipeline_end = renderer.index("bool updateFrameMapping(",
                                         record_pipeline_start)
    record_pipeline = renderer[record_pipeline_start:record_pipeline_end]
    require("resetRenderTarget(" not in record_pipeline,
            "present-time target changes must use fence-deferred destruction")
    require("pushConstants(s.easu_uniform.getGpuAddr()" in renderer and
            "pushConstants(s.rcas_uniform.getGpuAddr()" in renderer and
            "pushConstants(s.dithering_uniform.getGpuAddr()" in renderer,
            "dynamic post-process uniforms must be ordered on the GPU timeline")

    drain_start = renderer.index("void VideoRenderer::drainDecoderFrames()")
    drain_end = renderer.index("bool VideoRenderer::pollEvents()", drain_start)
    drain = renderer[drain_start:drain_end]
    require("s->q.waitIdle()" in drain and
            "releaseRetainedFrames(*s)" in drain and
            "s->fms.clear()" in drain,
            "decoder reset must retire GPU work, AVFrame refs, and stale NvMap mappings")

    print("video renderer hardware safety test passed")


if __name__ == "__main__":
    main()
