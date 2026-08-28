#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


pipeline = (ROOT / "src/stream/media_pipeline.cpp").read_text()
renderer = (ROOT / "src/stream/video_renderer.cpp").read_text()
renderer_header = (ROOT / "src/stream/video_renderer.h").read_text()

# Audio is the A/V master clock, but the Xbox hardware renderer has only a
# two-frame latest-frame handoff. Giving those slots per-frame release times can
# starve presentation when decode handoff runs before display presentation: the
# newly-ready head is evicted before present() observes it. Keep A/V timing as
# diagnostics/drop policy and never gate the renderer's bounded handoff on it.
require("video_renderer_->render(frame);" in pipeline,
        "media pipeline must hand decoded video to the renderer immediately")
require("video_renderer_->render(\n            frame," not in pipeline,
        "media pipeline must not pass an A/V-clock delay into the renderer")
require("presentation_delay_ns" not in renderer_header,
        "renderer API must stay independent of A/V-clock release deadlines")
require("pending_frame_ready_ns" not in renderer and
        "videoPresentationReady(" not in renderer,
        "two-slot hardware handoff must not gate pending frames by release time")

print("video presentation independence test passed")
