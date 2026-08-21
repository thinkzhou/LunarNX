#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("src/app/xbox_stream_session.cpp").read_text()
    media_pos = source.index("Media init begin")
    callbacks_pos = source.index("Peer callbacks installed")
    wait_pos = source.index("waitDataChannels(kDataChannelTimeout")

    require(callbacks_pos < wait_pos,
            "Peer callbacks must be installed before waitDataChannels can dispatch media")
    require(wait_pos < media_pos,
            "Media pipeline should be initialized only after data channels are ready")
    disable_pos = source.index("setMediaEnabled(false)")
    enable_pos = source.index("setMediaEnabled(true)")
    control_pos = source.index("control protocol start result=%s")
    keyframe_pos = source.index("requestVideoKeyframe(false)")
    startup_retry_pos = source.index("startup keyframe retry result=%s")
    answer_pos = source.index("transport_.setRemoteAnswer(answer);")
    send_ice_pos = source.index("session_client_.sendIceCandidates(")
    candidates_pos = source.index("transport_.addRemoteCandidates(remote_candidates);")

    require(callbacks_pos < disable_pos < wait_pos,
            "Media decode should be disabled before waitDataChannels drains startup RTP")
    require(wait_pos < media_pos < enable_pos,
            "Media pipeline should exist before media decode is enabled")
    require(control_pos < enable_pos < keyframe_pos,
            "Media decode must be enabled before requesting the initial keyframe")
    require(keyframe_pos < startup_retry_pos,
            "Startup keyframe retry must run after the initial keyframe request")
    require("kStartupKeyframeRetryInterval{1}" in source,
            "Startup keyframe retry should be faster than normal damage recovery")
    require("kRecoveryKeyframeInterval{1}" in source,
            "Damage recovery must retain its unified one-second request cadence")
    require("VideoRecoveryTransportRetry" not in source and
            "transport-only keyframe retry" not in source,
            "Recovery must not add an independent 200 ms transport-only PLI")
    require("video_rtp_missing_packets_detected" in source and
            "video_rtp_missing_packets - keyframe_missing_baseline" not in source,
            "Recovery must use a monotonic loss counter instead of subtracting a recoverable unsigned gauge")
    require("video callback begin" in Path("src/webrtc/peer_manager.cpp").read_text(),
            "PeerManager should keep bounded media callback diagnostics")
    require(answer_pos < candidates_pos,
            "Remote SDP answer should be set before remote ICE candidates are added")
    require(answer_pos < send_ice_pos,
            "Remote SDP answer must be set before local ICE is sent, matching XStreaming")
    require("transport_.setRemoteAnswer(answer, remote_candidates)" not in source,
            "Remote ICE candidates should not be appended into the SDP answer")

    input_start = source.index("void XboxStreamSession::startInputLoop(")
    input_end = source.index(
        "void XboxStreamSession::prepareInputForReconnect(", input_start)
    input_loop = source[input_start:input_end]
    run_loop_start = source.index("void XboxStreamSession::runLoop(")
    run_loop_end = source.index("void XboxStreamSession::controlLoop(")
    run_loop = source[run_loop_start:run_loop_end]
    require("gamepad_state = gamepad_.read()" in input_loop and
            "input_router_.route(gamepad_state)" in input_loop,
            "the input producer must own physical gamepad sampling and routing")
    require("transport_" not in input_loop,
            "the input producer must not call into libpeer/WebRTC")
    require("xinput_.encodeFrames(input_batch->frames)" in run_loop and
            "channels_.sendInputPacket" in run_loop,
            "the WebRTC owner loop must encode and send queued input frames")
    require("input_batch->reliable" in run_loop and
            "pending_input_batch" in run_loop and
            "consumeInputDeliveryResult" in run_loop,
            "transition batches must remain pending until the owner pump reports send completion")
    require("xinput_.reset()" not in source,
            "input sequence must reset with a new WebRTC association, not encoder state")
    loop_process_pos = run_loop.index("transport_.processEvents();")
    input_send_pos = run_loop.index("xinput_.encodeFrames(input_batch->frames)")
    require(input_send_pos < loop_process_pos,
            "Queued input must be sent before inbound media processing consumes the loop budget")
    require("kNetworkPumpInterval{2}" in source,
            "WebRTC must use a short pump cadence independent of input polling")
    require("kInputSampleInterval{8}" in source,
            "Xbox input must use an 8 ms producer cadence")
    require("kInputSnapshotInterval{16}" in source,
            "Xbox must retain the 62.5 Hz latest-state refresh cadence")
    require("next_network_tick += kNetworkPumpInterval" in source,
            "The WebRTC pump must maintain its own absolute cadence")
    require("next_input_tick" not in run_loop,
            "The WebRTC pump must not wait for the input producer cadence")
    metadata_pos = run_loop.index("xinput_.encodeMetadata(0)")
    control_started_pos = run_loop.index("control_started = true", metadata_pos)
    require(metadata_pos < control_started_pos,
            "metadata must be flushed before gamepad drafts can be enqueued")

    watchdog_start = run_loop.index("const bool pipeline_stalled")
    watchdog_end = run_loop.index(
        "if (std::chrono::duration_cast<std::chrono::seconds>(",
        watchdog_start,
    )
    watchdog = run_loop[watchdog_start:watchdog_end]
    require("if (pipeline_stalled) {" in watchdog and
            "else if (media_health.has_presented_video" not in watchdog and
            "health_recovery_attempts >= 1" in watchdog,
            "a repeated present of an old frame must not clear a pending recovery attempt")

    print("Xbox stream session order tests passed")


if __name__ == "__main__":
    main()
