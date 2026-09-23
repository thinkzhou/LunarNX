#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


controller = Path("src/ps/ps_stream_controller.cpp").read_text()
controller_header = Path("src/ps/ps_stream_controller.h").read_text()
session = Path("src/ps/ps_stream_session.cpp").read_text()
session_header = Path("src/ps/ps_stream_session.h").read_text()
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
monitor = controller.split("void PsStreamController::startVideoMonitor()", 1)[1].split(
    "bool PsStreamController::requestRecoveryIDR()", 1
)[0]
require("requestVideoRecovery(" in monitor and
        '"first rendered frame timeout", true' in monitor and
        "requestRecoveryIDR()" in monitor and
        '"first-video-recovery"' in monitor,
        "a first-frame timeout must reset the decoder gate and actively request "
        "a fresh IDR instead of only changing the status text")
require("kMaxFirstVideoRecoveryAttempts" in controller and
        "kFirstVideoRecoveryRetryInterval" in controller and
        'setState(app::StreamState::Error' in monitor and
        "!first_video_recovery_exhausted && media_" in monitor,
        "first-frame recovery must retry at a bounded cadence and terminate "
        "without continuing to send IDR requests forever")
require("Video received. Recovering decoder..." not in monitor and
        "video samples=" in monitor,
        "the profile parameter-set callback must not be described as a received "
        "video frame or decoder failure")
require("bool requestIDR();" in session_header and
        "bool PsStreamSession::requestIDR()" in session and
        "chiaki_session_request_idr(&session_)" in session and
        "CHIAKI_ERR_SUCCESS" in session.split(
            "bool PsStreamSession::requestIDR()", 1
        )[1].split("PsTransportStats", 1)[0],
        "IDR requests must propagate Chiaki send failures to the recovery loop")
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
