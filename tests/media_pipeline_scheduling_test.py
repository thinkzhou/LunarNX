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
renderer = (ROOT / "src/stream/video_renderer.cpp").read_text()
audio = (ROOT / "src/stream/audio_player.cpp").read_text()

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
require("constexpr size_t kMaxVideoQueuePackets = 2048" in pipeline,
        "Xbox access-unit queue must retain its established 2048-packet safety limit")
require("video_queue_.size() >= kMaxVideoQueuePackets" in pipeline,
        "queued video ingress must enforce the established safety limit")
require("return enqueueVideoPacket(data, len, timestamp);" in pipeline,
        "queued scheduling must preserve asynchronous network/decode isolation")
require("video_scheduling_.load(std::memory_order_acquire) ==" in pipeline and
        "VideoSchedulingMode::DirectLowLatency" in pipeline,
        "video ingress must dispatch direct low-latency samples separately")
require("return decodeVideoDirect(data, len, timestamp);" in pipeline,
        "direct low-latency samples must bypass the encoded queue")
require("std::recursive_mutex lifecycle_mutex_" in header,
        "direct decode and its renderer callback must serialize safely with shutdown")
require("std::atomic<VideoSchedulingMode> video_scheduling_" in header,
        "early transport callbacks must read scheduling state without a data race")
require("video_scheduling_.store(options.video_scheduling" in pipeline and
        "video_scheduling_.load(" in pipeline,
        "scheduling mode publication and callback dispatch must use atomic access")
require("bool decodeVideoDirect(" in header,
        "the direct path must be an explicit testable media operation")
require("reset = resetVideoDecoderForKeyframe()" in pipeline and
        "requestVideoRecovery(\"direct video decoder error\")" in pipeline,
        "direct decode failure must reset synchronously and request an IDR")
require("if(s->pending_frame)av_frame_free(&s->pending_frame);" in renderer and
        "s->pending_frame=keep;" in renderer,
        "shared renderer must replace its pending frame instead of buffering decoded frames")
require("VideoSchedulingMode" not in renderer and
        "VideoSchedulingMode" not in audio,
        "decoder sinks must remain independent of protocol scheduling policy")

print("media pipeline scheduling contract passed")
