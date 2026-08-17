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

    input_pos = source.index("gamepad_state = gamepad_.read()")
    loop_process_pos = source.index("transport_.processEvents();", source.index("while (streaming_"))
    require(input_pos < loop_process_pos,
            "Input must be sampled and sent before inbound media processing can consume the loop budget")
    require("kNetworkPumpInterval{2}" in source,
            "WebRTC must use a short pump cadence independent of input polling")
    require("kInputPollInterval{8}" in source,
            "Xbox input must poll at 125 Hz to keep sampling latency below 8 ms")
    require("const bool input_due" in source,
            "Input sampling must remain gated to its 8 ms cadence")
    require("next_input_tick += kInputPollInterval" in source,
            "Input polling must use an absolute cadence instead of work time plus a fixed sleep")
    require("next_network_tick += kNetworkPumpInterval" in source,
            "The WebRTC pump must maintain its own absolute cadence")
    require("std::min(next_network_tick, next_input_tick)" in source,
            "The stream loop must wake for networking before the next input deadline")
    require("sleepUntilCancelled(next_input_tick" not in source,
            "The WebRTC pump must not sleep until the next 8 ms input deadline")

    print("Xbox stream session order tests passed")


if __name__ == "__main__":
    main()
