#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VIEW = (ROOT / "src/ui/stream_view.cpp").read_text()
XBOX_SESSION = (ROOT / "src/app/xbox_stream_session.cpp").read_text()
PS_CONTROLLER = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
RENDERER = (ROOT / "src/stream/video_renderer.cpp").read_text()


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


require("kInputPollInterval{16}" in XBOX_SESSION and
        "next_input_tick += kInputPollInterval" in XBOX_SESSION and
        "gamepad_state = gamepad_.read()" in XBOX_SESSION,
        "Xbox input must be sampled by its protocol loop at normal gamepad cadence")
require("kPsInputInterval{8}" in PS_CONTROLLER and
        "input_thread_ = std::thread" in PS_CONTROLLER and
        "update();" in PS_CONTROLLER,
        "PS input must be sampled by its dedicated protocol input loop")
require("sleep_for(milliseconds(500))" in VIEW and
        "runtime_->update();" not in VIEW,
        "shared UI refresh must remain separate from protocol input sampling")
require("perf_.recordInputPacket()" in PS_CONTROLLER,
        "PS input sends must be visible in the performance counters")
require('dropDiagnosticLog(\n                "ps-input"' in PS_CONTROLLER and
        "input_transition_logs_ < 64" in PS_CONTROLLER,
        "button transitions need a bounded asynchronous real-hardware diagnostic")
require("startDropDiagnosticWriter()" in PS_CONTROLLER and
        "stopDropDiagnosticWriter()" in PS_CONTROLLER,
        "PS sessions must own the asynchronous diagnostic writer lifecycle")
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
