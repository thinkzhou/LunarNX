#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


codec = (ROOT / "src/stream/video_codec.h").read_text()
decoder = (ROOT / "src/stream/video_decoder.cpp").read_text()
pipeline = (ROOT / "src/stream/media_pipeline.h").read_text()

require("VideoPipelinePath::Xbox" in pipeline,
        "Xbox must remain the default media path")
xbox_parser = codec[codec.index("inspectXboxH264AccessUnit"):]
require("appendVideoNalType" not in xbox_parser,
        "Xbox H.264 inspection must not build NAL diagnostic strings")
require("? inspectVideoAccessUnit(video_codec_, data, len)" in decoder and
        ": inspectXboxH264AccessUnit(data, len)" in decoder,
        "decoder must dispatch Xbox access units to the H.264 fast path")
require("playstation_path && !au.has_vcl" in decoder and
        "playstation_path && parameter_sets_pending_" in decoder,
        "standalone parameter-set caching must stay on the PS path")
require("const VideoAccessUnitInfo* inspected_access_unit" in decoder and
        "if (!inspected_access_unit)" in decoder,
        "decoder must reuse access-unit metadata supplied by the Xbox queue")

media = (ROOT / "src/stream/media_pipeline.cpp").read_text()
require("&packet.access_unit" in media,
        "Xbox queue must pass its one-time inspection result to the decoder")

print("Xbox video fast-path tests passed")
