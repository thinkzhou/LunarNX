#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


controller = Path("src/ps/ps_stream_controller.cpp").read_text()
controller_header = Path("src/ps/ps_stream_controller.h").read_text()
bridge = Path("src/ps/ps_media_bridge.cpp").read_text()
adapter = Path("src/ps/chiaki_log_adapter.cpp").read_text()

require("media ready; requested initial IDR" in controller and
        "startVideoMonitor()" in controller,
        "PS startup must request a fresh IDR after the media pipeline is ready")
require("bridge_->setMediaReady()" in controller and
        "setMediaReady" in bridge and
        "pending_video_samples_" in bridge,
        "PS must retain video callbacks that arrive before MediaPipeline init")
require("stream_transport_connected_ = true" in controller and
        "if (!stream_transport_connected_.load()) continue" in controller,
        "the first-frame timeout must start after StreamConnection is ready, "
        "not while PSN DATA hole punching is still in progress")
require("hasVideoRecoveryRequest()" in controller and
        "requested IDR for video recovery" in controller and
        "if (media_ && media_->hasVideoRecoveryRequest()" in controller,
        "PS monitor must service recovery requests in every stream state")
require("last_recovery_request" in controller and
        "std::chrono::seconds(1)" in controller,
        "PS loading recovery requests must be rate limited")
require("video_monitor_thread_" in controller_header and
        "stopVideoMonitor();" in controller,
        "PS first-frame monitor must have explicit lifecycle cleanup")
require("media_.requestVideoRecovery(\"ps video sample loss\")" not in bridge and
        "decodeVideoPacket(data, size, pts)" in bridge,
        "successful Chiaki samples must not duplicate FEC recovery requests, "
        "and complete AUs must reach the media scheduling boundary")
require("noisy != NoisyLogKind::None) return" in adapter and
        "diagnosticLogMutex()" in adapter and
        "CHIAKI_LOG_DEBUG" not in adapter.split("log.level_mask =", 1)[1],
        "Switch Chiaki logging must suppress per-packet pressure and serialize file writes")

print("PS first-video recovery tests passed")
