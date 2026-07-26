#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    renderer = Path("src/stream/video_renderer.cpp").read_text()
    present_start = renderer.index("void VideoRenderer::present()")
    present_end = renderer.index("void VideoRenderer::prepareDecoderReset()", present_start)
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

    reset_start = renderer.index("void VideoRenderer::prepareDecoderReset()")
    reset_end = renderer.index("bool VideoRenderer::pollEvents()", reset_start)
    reset = renderer[reset_start:reset_end]
    require("s->q.waitIdle()" not in reset and
            "releaseRetainedFrames(*s)" not in reset and
            "s->fms.clear()" not in reset,
            "decoder recovery must not block on unrelated GPU work or invalidate fenced mappings")
    require("av_frame_free(&s->pending_frame)" in reset,
            "decoder recovery may release only the frame that has not reached the GPU")

    print("video renderer hardware safety test passed")


if __name__ == "__main__":
    main()
