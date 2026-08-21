#!/usr/bin/env python3
"""Source contracts for NVTEGRA zero-copy frame ownership."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    renderer = (ROOT / "src/stream/video_renderer.cpp").read_text()
    pipeline = (ROOT / "src/stream/media_pipeline.cpp").read_text()

    require("std::array<AVFrame*,kPendingFrameCapacity> pending_frames" in renderer and
            "kPendingFrameCapacity=2" in renderer and
            "AVFrame* current_frame=nullptr" in renderer,
            "Zero-copy renderer should retain a two-frame pending queue and current frame")
    require("std::array<AVFrame*, brls::FRAMEBUFFERS_COUNT> submitted_frames" in renderer,
            "Submitted NVDEC frames should remain retained across the command ring")
    require("av_frame_ref(keep,f)" in renderer and
            "enqueuePendingFrame(*s,keep)" in renderer,
            "Renderer should retain the borrowed NVTEGRA frame in the bounded queue")
    require("s->current_frame=dequeuePendingFrame(*s)" in renderer,
            "Present should consume the oldest decoded frame")
    require("s->present_ring->begin(s->present_cb)" in renderer and
            "next_submitted_frame" in renderer,
            "Frame retirement must follow the existing Deko3D command-ring fence")

    require("dk::MemBlockMaker{s.dev,size}" in renderer and
            ".setStorage(address)" in renderer,
            "NvMap should use Moonlight-style external deko3d storage")
    require("bindUniformBuffer(DkStage_Fragment,0,s.tu.getGpuAddr(),s.tu.getSize())" in renderer,
            "NV12 shader should bind the transformation uniform like Moonlight-Switch")
    require("chroma_delta>=size" in renderer and
            "luma_delta>=size" in renderer,
            "Renderer should reject plane offsets outside the NvMap")
    require("luma_end>size" in renderer and
            "chroma_end>size" in renderer,
            "Renderer should validate both image layouts against the NvMap")
    require("DkImageFlags_PitchLinear" in renderer and
            "DkTileSize_TwoGobs" in renderer,
            "Image layouts must match the NVTEGRA linear/block-linear allocation")
    require("mapping.lu.initialize(mapping.ll,mapping.mb,luma_offset)" in renderer,
            "The luma image must start at the mapped frame's real plane offset")
    require("s->fms.size() >= 8" not in renderer,
            "NvMap mappings should follow Moonlight and remain cached for the bounded decoder pool")

    reset_start = pipeline.index("bool MediaPipeline::resetVideoDecoderForKeyframe()")
    reset_end = pipeline.index("void MediaPipeline::processVideoPacket", reset_start)
    reset = pipeline[reset_start:reset_end]
    require(reset.index("video_renderer_->prepareDecoderReset()") <
            reset.index("video_decoder_->resetForKeyframe()"),
            "Renderer must release only its unsubmitted frame before FFmpeg flushes its DPB")

    shutdown = renderer.index("void VideoRenderer::shutdown()")
    wait_idle = renderer.index("s->q.waitIdle()", shutdown)
    clear_mappings = renderer.index("s->fms.clear()", shutdown)
    require(wait_idle < clear_mappings,
            "Shutdown should wait for GPU idle before destroying mappings")

    present_start = renderer.index("void VideoRenderer::present()")
    present_end = renderer.index("bool VideoRenderer::prepareDecoderReset()", present_start)
    require("s->q.waitIdle()" not in renderer[present_start:present_end],
            "Borealis draw must not wait for queue idle before endFrame/presentImage")
    present = renderer[present_start:present_end]
    require("decoder_reset_drain_steps>s->submitted_frames.size()" in present and
            present.index("present_ring->begin") < present.index("s->fms.clear()"),
            "Decoder reset must retire every video fence before releasing NVDEC mappings")

    print("Zero-copy frame lifetime tests passed")


if __name__ == "__main__":
    main()
