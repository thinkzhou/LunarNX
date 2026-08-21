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
require("RealtimeQueued" in header and "DirectLowLatency" in header and
        "BoundedLowLatency" in header,
        "scheduling contract must describe queued, direct, and bounded behavior")
require("VideoSchedulingMode video_scheduling" in header,
        "media options must carry the selected video scheduling mode")
require("VideoSchedulingMode::RealtimeQueued" in xbox,
        "Xbox controller must explicitly select isolated realtime queue scheduling")
require("VideoSchedulingMode::BoundedLowLatency" in ps,
        "PlayStation controller must default to bounded low-latency scheduling")
require("LUNARNX_PS_DIRECT_VIDEO" in ps and
        "VideoSchedulingMode::DirectLowLatency" in ps,
        "PlayStation direct scheduling must remain available for development A/B")
require("std::clamp<size_t>" in ps and "(fps_ + 9) / 10" in ps and
        "8 * 1024 * 1024" in ps and
        "std::chrono::milliseconds(100)" in ps,
        "PlayStation bounded scheduling must select a frame-rate-scaled AU/8 MiB/100 ms budget")
require("#include <chiaki/" not in header and "libpeer/" not in header,
        "shared scheduling contract must not expose transport-library types")
require("constexpr size_t kMaxVideoQueuePackets = 2048" in pipeline,
        "Xbox access-unit queue must retain its established 2048-packet safety limit")
require("? video_queue_limits_.max_packets" in pipeline and
        ": kMaxVideoQueuePackets" in pipeline and
        "evaluateBoundedVideoAdmission(" in pipeline and
        "realtimeVideoCapacityExceeded(" in pipeline,
        "queued video ingress must preserve Xbox limits and select bounded limits by mode")
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
require("kPendingFrameCapacity=2" in renderer and
        "enqueuePendingFrame(*s,keep);" in renderer and
        "if(s.pending_count==s.pending_frames.size())" in renderer,
        "shared renderer must retain a bounded two-frame decoded queue")
require("VideoSchedulingMode" not in renderer and
        "VideoSchedulingMode" not in audio,
        "decoder sinks must remain independent of protocol scheduling policy")
require("decoded_pending_drop_oldest" in (ROOT / "src/stream/perf_stats.h").read_text() and
        "unique_video_frames_presented" in (ROOT / "src/stream/perf_stats.h").read_text() and
        "present_gap_max_us" in (ROOT / "src/stream/perf_stats.h").read_text() and
        "10s decoded_queue_drop_oldest" in (ROOT / "src/ps/ps_media_bridge.cpp").read_text(),
        "decoded-to-present visibility must be exposed through low-frequency counters")

print("media pipeline scheduling contract passed")
