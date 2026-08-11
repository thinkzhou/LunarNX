#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


options = (ROOT / "src/stream/media_pipeline.h").read_text()
ps_controller = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
pipeline = (ROOT / "src/stream/media_pipeline.cpp").read_text()

require(
    "bool hold_non_target_startup_frames = false;" in options,
    "Xbox/default media must present a non-target startup frame immediately",
)
require(
    "media_opts.hold_non_target_startup_frames = true;" in ps_controller,
    "PlayStation must retain its bounded startup resolution hold",
)
require(
    "options.hold_non_target_startup_frames" in pipeline,
    "MediaPipeline must pass the platform startup policy to VideoRenderer",
)

print("Platform resolution startup policy tests passed")
