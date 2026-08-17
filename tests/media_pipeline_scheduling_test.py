#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


header = (ROOT / "src/stream/media_pipeline.h").read_text()
xbox = (ROOT / "src/app/stream_controller.cpp").read_text()
ps = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
pipeline = (ROOT / "src/stream/media_pipeline.cpp").read_text()

require("enum class VideoSchedulingMode" in header,
        "media pipeline must expose a protocol-neutral scheduling mode")
require("RealtimeQueued" in header and "DirectLowLatency" in header,
        "scheduling contract must describe queued realtime and direct low-latency behavior")
require("VideoSchedulingMode video_scheduling" in header,
        "media options must carry the selected video scheduling mode")
require("VideoSchedulingMode::RealtimeQueued" in xbox,
        "Xbox controller must explicitly select isolated realtime queue scheduling")
require("VideoSchedulingMode::DirectLowLatency" in ps,
        "PlayStation controller must explicitly select direct low-latency scheduling")
require("#include <chiaki/" not in header and "libpeer/" not in header,
        "shared scheduling contract must not expose transport-library types")
require("constexpr size_t kRealtimeVideoQueueFrames = 3" in pipeline,
        "Xbox realtime access-unit backlog must be explicitly bounded to three frames")
require("video_queue_.size() >= kRealtimeVideoQueueFrames" in pipeline,
        "queued video ingress must enforce the realtime frame bound")
require("return enqueueVideoPacket(data, len, timestamp);" in pipeline,
        "queued scheduling must preserve asynchronous network/decode isolation")

print("media pipeline scheduling contract passed")
