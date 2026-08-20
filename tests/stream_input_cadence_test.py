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


input_loop_start = XBOX_SESSION.index("void XboxStreamSession::startInputLoop(")
input_loop_end = XBOX_SESSION.index(
    "void XboxStreamSession::prepareInputForReconnect(", input_loop_start)
input_loop = XBOX_SESSION[input_loop_start:input_loop_end]
run_loop_start = XBOX_SESSION.index("void XboxStreamSession::runLoop(")
run_loop_end = XBOX_SESSION.index("void XboxStreamSession::controlLoop(")
run_loop = XBOX_SESSION[run_loop_start:run_loop_end]

require("kInputSampleInterval{8}" in XBOX_SESSION and
        "input_thread_ = std::thread" in XBOX_SESSION and
        "gamepad_state = gamepad_.read()" in input_loop and
        "input_router_.route(gamepad_state)" in input_loop,
        "Xbox must sample and route controller input on its dedicated 8 ms producer")
require("transport_" not in input_loop,
        "the Xbox input producer must not call WebRTC/libpeer")
require("transport_.processEvents()" in run_loop and
        "xinput_.encodeFrames(input_batch->frames)" in run_loop and
        "channels_.sendInputPacket" in run_loop,
        "the Xbox WebRTC owner loop must encode and send sampled frames")
require("input_accumulator_.peekBatch()" in run_loop and
        "input_accumulator_.commitBatch(*input_batch)" in run_loop and
        "input-transition-overflow" in run_loop,
        "Xbox transitions must use transactional batching and reconnect on overflow")
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
