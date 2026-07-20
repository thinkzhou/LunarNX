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
    require("next_input_tick += kInputPollInterval" in source,
            "Input polling must use an absolute cadence instead of work time plus a fixed sleep")
    require("sleepUntilCancelled(next_input_tick" in source,
            "The stream loop must sleep to the next 16 ms input deadline")

    print("Xbox stream session order tests passed")


if __name__ == "__main__":
    main()
