#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VIEW = (ROOT / "src/ui/stream_view.cpp").read_text()
PS_CONTROLLER = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
RENDERER = (ROOT / "src/stream/video_renderer.cpp").read_text()


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


require("sleep_for(milliseconds(8))" in VIEW,
        "stream input must be sampled fast enough to observe normal button taps")
require("now - last_stats < milliseconds(500)" in VIEW,
        "slow UI/performance refresh must remain separate from input sampling")
require("sleep_for(milliseconds(500))" not in VIEW,
        "input and performance refresh must not share the old 2 Hz cadence")
require("perf_.recordInputPacket()" in PS_CONTROLLER,
        "PS input sends must be visible in the performance counters")
require('diagnosticLog("ps-input"' in PS_CONTROLLER,
        "button transitions need a bounded real-hardware diagnostic")
require("std::shared_lock<std::shared_mutex> operation_lock(stream_operation_mutex_)" in
        PS_CONTROLLER,
        "input and presentation must share lifetime protection without blocking each other")
require(PS_CONTROLLER.count(
            "std::shared_lock<std::shared_mutex> operation_lock(stream_operation_mutex_)") >= 2,
        "both input and presentation hot paths must use shared lifetime access")
require("return frame_id == 1;" in RENDERER,
        "successful hardware render probes must not cause periodic SD-card stalls")
require("frame_id % 120" not in RENDERER,
        "periodic success-path hardware probe logging must stay disabled")

print("Stream input cadence test passed")
