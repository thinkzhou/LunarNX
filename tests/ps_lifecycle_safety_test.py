#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER_H = (ROOT / "src/ps/ps_stream_controller.h").read_text()
CONTROLLER = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
BRIDGE_H = (ROOT / "src/ps/ps_media_bridge.h").read_text()
LOG_ADAPTER = (ROOT / "src/ps/chiaki_log_adapter.cpp").read_text()


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


require("std::shared_mutex stream_operation_mutex_" in CONTROLLER_H,
        "session graph lifetime must use shared/exclusive protection")
require(CONTROLLER.count(
            "std::unique_lock<std::shared_mutex> operation_lock(stream_operation_mutex_)") >= 2,
        "startup and shutdown must take exclusive lifetime ownership")
require(CONTROLLER.count(
            "std::shared_lock<std::shared_mutex> operation_lock(stream_operation_mutex_)") >= 2,
        "input and presentation must remain concurrent while holding lifetime access")
require("std::lock_guard<std::mutex> lock(remote_connector_mutex_)" in CONTROLLER and
        "std::lock_guard<std::mutex> remote_lock(remote_connector_mutex_)" in CONTROLLER,
        "remote connector cancellation and reset must stay serialized")
require("std::atomic<uint64_t> next_video_pts_ns_" in BRIDGE_H,
        "audio/video callback PTS handoff must be race-free")
require("new ChiakiLogContext" not in LOG_ADAPTER and
        "log.user = const_cast<char*>(name)" in LOG_ADAPTER,
        "concurrent Chiaki logging must not leak heap-owned callback contexts")
require('NoisyLogKind::SendBufferOverflow' in LOG_ADAPTER and
        'dropDiagnosticLog(\n                "chiaki-flow"' in LOG_ADAPTER and
        "count <= 4 || isPowerOfTwo(count)" in LOG_ADAPTER,
        "send-buffer overflow spam must be aggregated asynchronously")
require("if (!stream_transport_connected_.load()) continue;" in CONTROLLER,
        "first-video timeout must not run before StreamConnection is connected")

print("PS lifecycle safety test passed")
