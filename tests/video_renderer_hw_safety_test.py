#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    renderer = Path("src/stream/video_renderer.cpp").read_text()
    renderer_header = Path("src/stream/video_renderer.h").read_text()
    media_header = Path("src/stream/media_pipeline.h").read_text()
    render_start = renderer.index("bool VideoRenderer::render(const VideoFrame&frame)")
    render_end = renderer.index("void VideoRenderer::present()", render_start)
    render = renderer[render_start:render_end]

    require("getGpuMutex()" not in render,
            "the decoder hot path must not contend with the Borealis GPU frame lock")
    require("std::lock_guard<std::mutex> lock(s->render_mutex)" in render,
            "decoded-frame handoff must remain protected by the renderer mutex")

    present_start = renderer.index("void VideoRenderer::present()")
    present_end = renderer.index("bool VideoRenderer::prepareDecoderReset()", present_start)
    present = renderer[present_start:present_end]

    require("s->present_ring->begin(s->present_cb," in present and
            "kCommandRingWaitTimeoutNs" in present,
            "hardware presentation must use a bounded command-ring fence wait")
    require("s->present_ring->begin(s->present_cb)" not in present,
            "hardware presentation must never use an unbounded fence wait")
    require("RenderFault::CommandFenceTimeout" in renderer and
            "kCommandRingTimeoutFaultThreshold" in renderer,
            "a persistent command-ring timeout must surface as renderer recovery")
    require("kCommandRingTimeoutFaultThreshold = 45" in renderer,
            "renderer recovery should start at 750 ms, leaving 250 ms before the 1 s watchdog")
    require("consecutive_update_ring_timeouts" in renderer and
            "consecutive_present_ring_timeouts" in renderer,
            "descriptor progress must not hide consecutive presentation timeouts")
    require("enum class VideoRenderStage" in media_header and
            "setProgressSink" in renderer_header and
            "setRenderStage" in renderer,
            "renderer progress must be published without waiting for the stuck render thread")
    require("VideoRenderStage::WaitingGpuMutex" in present and
            "VideoRenderStage::WaitingRenderMutex" in present and
            "VideoRenderStage::PresentFence" in present and
            "VideoRenderStage::SubmitCommands" in present,
            "renderer diagnostics must distinguish locks, fences, and command submission")
    require("kCommandRingDiagnosticThreshold = 8" in renderer and
            '"fence-stall' in renderer and '"fence-resumed' in renderer,
            "meaningful transient fence stalls must be logged without per-frame spam")
    require("duration_ms=%llu" in renderer and "queue_error=%d" in renderer and
            "mappings=%zu" in renderer and "pending=%zu" in renderer,
            "fence incidents must capture duration and GPU resource state")
    require("~BoundedCmdMemRing() { memory_.destroy(); }" in renderer,
            "the bounded command ring must release its command-memory slice")
    require("s->q.waitIdle()" not in present,
            "present must not block before Borealis endFrame/presentImage")
    require("submitted_frames" in present and "completed_frame" in present,
            "GPU-used NVDEC frames must be retained until their command slice retires")
    require("s->next_submitted_frame=(submitted_index+1)%s->submitted_frames.size()" in present,
            "frame retirement must advance with the command-ring slice")
    require("luma_delta" in renderer and "chroma_delta" in renderer,
            "NVTEGRA plane offsets must be computed from the NvMap base")
    require("makeNv12TextureGeometry" in renderer and
            ".setDimensions(geometry.luma_width,geometry.luma_height,1)" in renderer and
            ".setDimensions(geometry.chroma_width,geometry.chroma_height,1)" in renderer,
            "NVTEGRA descriptors must expose visible dimensions, not aligned padding")
    require("cropToMappedSurface" not in renderer,
            "visible descriptors must not be rescaled back across aligned padding")
    require("retireCompletedTargets(*s, submitted_index)" in present and
            "present_slice_active=true" in present,
            "post-process targets must retire against the same command-ring fence")
    require("pending_slice_mask" in renderer and
            "deferResetRenderTarget" in renderer,
            "GPU-used render targets must not be destroyed immediately")
    require("resolution_transition.isTransitioning()" in present and
            "resolution_transition_frame" in present and
            "resolution_transition_drain_steps" in present,
            "resolution changes must drain old GPU work before mapping a new geometry")
    transition_start = present.index(
        "if(s->resolution_transition.isTransitioning())")
    transition_end = present.index(
        "if(s->pending_count==0&&!s->current_frame)", transition_start)
    transition = present[transition_start:transition_end]
    transition_clear = transition.index("s->fms.clear()")
    transition_release = transition.index("av_frame_free(&s->current_frame)")
    require(transition_clear < transition_release,
            "old Deko3D mappings must be destroyed before their NVDEC frames")
    require("resolution_transition_drain_steps>s->submitted_frames.size()" in
            transition and "s->q.waitIdle()" not in transition,
            "resolution transition must retire every command slice without queue idle")
    require("startupCandidateReady" in present,
            "a non-target startup frame must eventually be displayed")
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

    reset_start = renderer.index("bool VideoRenderer::prepareDecoderReset()")
    reset_end = renderer.index("bool VideoRenderer::pollEvents()", reset_start)
    reset = renderer[reset_start:reset_end]
    require("s->q.waitIdle()" not in reset and
            "releaseRetainedFrames(*s)" not in reset and
            "s->fms.clear()" not in reset,
            "the decoder thread must not operate on UI-owned GPU resources")
    require("decoder_reset_cv.wait_for" in reset,
            "decoder recovery must wait for the UI command-ring handoff")

    shutdown_start = renderer.index("void VideoRenderer::shutdown()", reset_end)
    shutdown_end = renderer.index("\n}\n\n}\n#else", shutdown_start)
    shutdown = renderer[shutdown_start:shutdown_end]
    require("gpu_quarantine_required_" in renderer_header and
            "phase=gpu-quarantine" in shutdown,
            "a persistently stalled GPU context must be quarantined during shutdown")
    require(shutdown.index("phase=gpu-quarantine") <
            shutdown.index("s->q.waitIdle()"),
            "fatal command-fence shutdown must not reach the unbounded queue wait")

    print("video renderer hardware safety test passed")


if __name__ == "__main__":
    main()
