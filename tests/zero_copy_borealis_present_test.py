#!/usr/bin/env python3
"""Source contracts for flicker-free Borealis zero-copy presentation."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    pipeline_h = (ROOT / "src/stream/media_pipeline.h").read_text()
    pipeline = (ROOT / "src/stream/media_pipeline.cpp").read_text()
    controller_h = (ROOT / "src/app/stream_controller.h").read_text()
    controller = (ROOT / "src/app/stream_controller.cpp").read_text()
    stream_view = (ROOT / "src/ui/stream_view.cpp").read_text()
    renderer = (ROOT / "src/stream/video_renderer.cpp").read_text()

    require("void presentVideoFrame();" in pipeline_h and
            "void MediaPipeline::presentVideoFrame()" in pipeline,
            "The UI frame loop needs an explicit media presentation entry point")
    require("void presentVideoFrame();" in controller_h and
            "void StreamController::presentVideoFrame()" in controller,
            "StreamController should expose the presentation entry point to the view")
    require("class HardwareVideoView" in stream_view and
            "ctrl_->presentVideoFrame();" in stream_view,
            "Zero-copy video should be redrawn from a Borealis View every UI frame")
    require("usesZeroCopyRender(ctrl_->getDefaultVideoBackend())" in stream_view,
            "Only zero-copy mode should install the hardware video view")

    require("if (rendered) presentFrame(generation);" not in pipeline and
            "void MediaPipeline::presentFrame(" not in pipeline,
            "Decoded frames must not enqueue an independent Borealis presentation")
    require("AVFrame* current_frame=nullptr" in renderer,
            "The renderer should retain the latest frame for repeated UI draws")
    require("DkFence" not in renderer and ".fence.wait(" not in renderer,
            "Borealis draw must not wait on per-frame GPU fences")
    require("s->q.submitCommands(s->direct_cl)" in renderer,
            "The direct path should reuse a static Moonlight-style command list")
    require("updateFrameMapping(*s,s->current_frame)" in renderer,
            "NvMap descriptors should update from the displayed frame on the Borealis thread")

    print("Zero-copy Borealis presentation tests passed")


if __name__ == "__main__":
    main()
