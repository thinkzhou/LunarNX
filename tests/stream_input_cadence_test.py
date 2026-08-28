#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VIEW = (ROOT / "src/ui/stream_view.cpp").read_text()
XBOX_SESSION = (ROOT / "src/app/xbox_stream_session.cpp").read_text()
XBOX_CHANNEL = (ROOT / "src/app/xbox_channel_manager.cpp").read_text()
DATA_CHANNELS = (ROOT / "src/webrtc/xstreaming_data_channels.h").read_text()
PEER_MANAGER = (ROOT / "src/webrtc/peer_manager.cpp").read_text()
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
        "xinput_.encode(input_snapshot->state)" in run_loop and
        "channels_.sendInputPacket" in run_loop,
        "the Xbox WebRTC owner loop must encode and send one current-state frame")
process_events_start = PEER_MANAGER.index("void PeerManager::processEvents()")
process_events = PEER_MANAGER[process_events_start:]
require("drainOutboundCommands" in process_events and
        "realtime_input_only" in process_events and
        process_events.index("realtime_input_only") <
            process_events.index("peer_connection_loop(pc_)"),
        "replaceable input must drain before the potentially slow inbound peer loop")
require("input_accumulator_.peekLatest()" in run_loop and
        "input_accumulator_.peekTransition(input_now_ns)" in run_loop and
        "input_accumulator_.commitLatest(*input_snapshot)" in run_loop and
        "input_accumulator_.commitTransition(*input_snapshot)" in run_loop and
        "pending_input_batch" not in run_loop and
        "consumeInputDeliveryResult" not in run_loop and
        "input-transition-overflow" not in run_loop,
        "Xbox input must use latest-state delivery plus the bounded edge journal")
require("{\"control\", \"controlV1\", 0, true, -1}" in DATA_CHANNELS and
        "{\"input\", \"1.0\", 2, true, -1}" in DATA_CHANNELS and
        "{\"message\", \"messageV1\", 4, true, -1}" in DATA_CHANNELS and
        "{\"chat\", \"chatV1\", 6, true, -1}" in DATA_CHANNELS and
        "ch.ordered ? DATA_CHANNEL_RELIABLE" in PEER_MANAGER and
        "sendInputTransitionPacket" in XBOX_CHANNEL and
        "sendTransitionInputData(data, len)" in XBOX_CHANNEL and
        "sendLatestInputData(data, len)" in XBOX_CHANNEL,
        "Xbox v1 input must retain Green-NX's reliable ordered channel with bounded edge delivery")
accumulator = (ROOT / "src/input/xbox_input_accumulator.cpp").read_text()
accumulator_header = (
    ROOT / "src/input/xbox_input_accumulator.h").read_text()
require("kInputHeartbeatInterval" not in XBOX_SESSION and
        "heartbeat_due" not in XBOX_SESSION and
        "kInputSnapshotInterval" not in XBOX_SESSION and
        "latest_dirty_ = true" in accumulator and
        "kTransitionLifetimeNs = 50'000'000" in accumulator_header and
        "transitions_" in accumulator,
        "Xbox input must publish every 8 ms and expire digital edges after 50 ms")
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
ps_writer_start = PS_CONTROLLER.index("startDropDiagnosticWriter()")
ps_remote_connect = PS_CONTROLLER.index("connector->connect(")
ps_video_ready_callback = PS_CONTROLLER.index("media_->setVideoReadyCallback(")
require(ps_writer_start > ps_remote_connect and
        ps_writer_start > ps_video_ready_callback,
        "PS diagnostics must not consume a Switch thread until the remote "
        "session has connected and delivered its first displayable frame")
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
