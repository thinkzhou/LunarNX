#!/usr/bin/env python3
"""Source contracts for NVTEGRA zero-copy frame ownership."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    renderer = (ROOT / "src/stream/video_renderer.cpp").read_text()

    require("AVFrame* pending_frame=nullptr" in renderer and
            "AVFrame* current_frame=nullptr" in renderer,
            "Zero-copy renderer should retain pending and currently displayed frames")
    require("std::array<AVFrame*, brls::FRAMEBUFFERS_COUNT + 1> retired_frames" in renderer,
            "Old NVDEC frames should remain retained across the Borealis framebuffer ring")
    require("av_frame_ref(keep,f)" in renderer and
            "pending_frame=keep" in renderer,
            "Renderer should retain the borrowed NVTEGRA frame")
    require("s->current_frame=s->pending_frame" in renderer and
            "s->pending_frame=nullptr" in renderer,
            "Present should retain the latest frame for repeated UI draws")
    require("DkFence" not in renderer and ".fence.wait(" not in renderer,
            "The Moonlight-style Borealis draw path must not depend on per-frame GPU fences")
    require("retained_frames" not in renderer,
            "Unfenced retained-frame timing assumptions should be removed")

    require("dk::MemBlockMaker{s.dev,size}" in renderer and
            ".setStorage(address)" in renderer,
            "NvMap should use Moonlight-style external deko3d storage")
    require("bindUniformBuffer(DkStage_Fragment,0,s.tu.getGpuAddr(),s.tu.getSize())" in renderer,
            "NV12 shader should bind the transformation uniform like Moonlight-Switch")
    require("chroma_offset>=size" in renderer,
            "Renderer should reject chroma offsets outside the NvMap")
    require("s.ll.getSize()>size" in renderer and
            "chroma_offset+s.cll.getSize()>size" in renderer,
            "Renderer should validate both image layouts against the NvMap")
    require("s->fms.size() >= 8" not in renderer,
            "NvMap mappings should follow Moonlight and remain cached for the bounded decoder pool")

    shutdown = renderer.index("void VideoRenderer::shutdown()")
    wait_idle = renderer.index("s->q.waitIdle()", shutdown)
    clear_mappings = renderer.index("s->fms.clear()", shutdown)
    require(wait_idle < clear_mappings,
            "Shutdown should wait for GPU idle before destroying mappings")

    print("Zero-copy frame lifetime tests passed")


if __name__ == "__main__":
    main()
