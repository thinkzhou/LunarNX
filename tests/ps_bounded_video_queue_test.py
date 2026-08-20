#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


header = (ROOT / "src/stream/media_pipeline.h").read_text()
impl = (ROOT / "src/stream/media_pipeline.cpp").read_text()
policy = (ROOT / "src/stream/bounded_video_queue_policy.h").read_text()
controller = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
bridge = (ROOT / "src/ps/ps_media_bridge.cpp").read_text()

require("struct VideoQueueLimits" in header and
        "size_t max_packets = 3" in header and
        "size_t max_bytes = 8 * 1024 * 1024" in header and
        "std::chrono::milliseconds max_age{50}" in header,
        "bounded queue limits must be explicit configuration")
require("std::chrono::steady_clock::time_point enqueued_at" in header and
        "#if LUNARNX_DROP_DIAGNOSTIC_LOG\n        std::chrono::steady_clock::time_point enqueued_at" not in header,
        "enqueue time must be available to release bounded scheduling")
require("VideoSchedulingMode::BoundedLowLatency" in controller and
        "LUNARNX_PS_DIRECT_VIDEO" in controller,
        "PS must default bounded while retaining a compile-time direct A/B")
require("evaluateBoundedVideoAdmission(" in impl and
        "queue.oldest_age >= max_age" in policy,
        "enqueue must enforce a monotonic oldest-AU age budget")
require("std::chrono::steady_clock::now() - packet.enqueued_at >=\n                video_queue_limits_.max_age" in impl and
        "beginHardVideoRecovery(\"video queue age\", true)" in impl,
        "worker must reject an AU that expires after pop and decoder reset")
require("queue.packets >= max_packets" in policy and
        "incoming_bytes > max_bytes" in policy,
        "enqueue must enforce bounded AU count and bytes")
require("admission == BoundedVideoAdmission::RejectOversize" in impl and
        "intentional_drop = true" in impl,
        "oversized bounded AUs must recover without entering the queue")
require("beginHardVideoRecoveryLocked(" in impl and
        "!packet.contains_idr ||" in impl,
        "overflow must enter hard recovery instead of retaining dependent P frames")
require("BoundedVideoAdmission::DropDependent" in impl and
        "return true;" in impl,
        "waiting P frames must be intentional successful callback drops")
require("packet.recovery_epoch = video_recovery_epoch_.load()" in impl,
        "random-access recovery candidates must join the current epoch")
require("decodeVideoPacket(data, size, pts)" in bridge,
        "Chiaki callback must hand complete AUs to the scheduling boundary")
require("video_decoder_->decode" not in bridge,
        "Chiaki callback must not execute decoder work")

print("PS bounded video queue contract passed")
