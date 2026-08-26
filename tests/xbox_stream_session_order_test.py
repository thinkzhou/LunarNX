#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("src/app/xbox_stream_session.cpp").read_text()
    session_header = Path("src/app/xbox_stream_session.h").read_text()
    channel_source = Path("src/app/xbox_channel_manager.cpp").read_text()
    media_header = Path("src/stream/media_pipeline.h").read_text()
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
    thread_pos = source.index("stream_thread_ = std::thread")
    ready_wait_pos = source.index("while (!media_startup_ready_.load()")
    streaming_callback_pos = source.index("if (callbacks.on_streaming)")
    ready_store_pos = source.index("media_startup_ready_ = true")
    require("std::atomic<bool> media_startup_ready_{false}" in session_header,
            "session startup needs an explicit media-ready state")
    require(thread_pos < ready_wait_pos < streaming_callback_pos,
            "the loading page must wait for control/media readiness before entering StreamView")
    require(enable_pos < ready_store_pos,
            "media startup may become ready only after the RTP gate is enabled")
    require("phase=media-startup-timeout" in source and
            "video_rtp=%u audio_rtp=%u" in source,
            "release builds must preserve sparse evidence for an all-zero startup")
    handshake_timeout = channel_source[
        channel_source.index("if (elapsed >= kHandshakeTimeout)"):
        channel_source.index("std::this_thread::sleep_for", channel_source.index("if (elapsed >= kHandshakeTimeout)"))
    ]
    require("return false;" in handshake_timeout and
            "phase=handshake-timeout" in handshake_timeout,
            "control startup must not claim success without a real HandshakeAck")
    require(keyframe_pos < startup_retry_pos,
            "Startup keyframe retry must run after the initial keyframe request")
    require("kStartupKeyframeRetryInterval{1}" in source,
            "Startup keyframe retry should keep its bounded one-second cadence")
    require('include "video_recovery_request_policy.h"' in source and
            "recovery_pli_policy.shouldRequest" in source and
            "recovery_pli_policy.recordAttempt" in source and
            "kRecoveryKeyframeInterval" not in source,
            "Damage recovery must use the bounded immediate/300/800 ms PLI policy")
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
    require("xinput_.encode(input_snapshot->state)" in run_loop and
            "channels_.sendInputPacket" in run_loop,
            "the WebRTC owner loop must encode and send current input snapshots")
    require("input_accumulator_.peekLatest()" in run_loop and
            "input_accumulator_.commitLatest(*input_snapshot)" in run_loop and
            "pending_input_batch" not in run_loop and
            "consumeInputDeliveryResult" not in run_loop,
            "input delivery must use one replaceable latest-state mailbox")
    require("xinput_.reset()" not in source,
            "input sequence must reset with a new WebRTC association, not encoder state")
    loop_process_pos = run_loop.index("transport_.processEvents();")
    input_send_pos = run_loop.index("xinput_.encode(input_snapshot->state)")
    require(input_send_pos < loop_process_pos,
            "Queued input must be sent before inbound media processing consumes the loop budget")
    require("kNetworkPumpInterval{2}" in source,
            "WebRTC must use a short pump cadence independent of input polling")
    require("kInputSampleInterval{8}" in source,
            "Xbox input must use an 8 ms producer cadence")
    require("kInputSnapshotInterval" not in source,
            "Xbox must send at the same 8 ms cadence used to sample input")
    require("next_network_tick += kNetworkPumpInterval" in source,
            "The WebRTC pump must maintain its own absolute cadence")
    require("next_input_tick" not in run_loop,
            "The WebRTC pump must not wait for the input producer cadence")
    metadata_pos = run_loop.index("xinput_.encodeMetadata(0)")
    control_started_pos = run_loop.index("control_started = true", metadata_pos)
    require(metadata_pos < control_started_pos,
            "metadata must be flushed before gamepad drafts can be enqueued")

    watchdog_start = run_loop.index("VideoWatchdogObservation")
    watchdog_end = run_loop.index(
        "if (std::chrono::duration_cast<std::chrono::seconds>(",
        watchdog_start,
    )
    watchdog = run_loop[watchdog_start:watchdog_end]
    require("decideVideoWatchdogAction" in watchdog and
            "VideoWatchdogAction::RecoverRenderer" in watchdog and
            "media_.requestRendererRecovery" in watchdog,
            "a present-only stall must use renderer recovery")
    require("VideoWatchdogAction::StopStream" in watchdog and
            "Video presentation stalled" in watchdog,
            "a failed renderer recovery must leave the stream without a crash loop")
    require("renderer_stage" in media_header and
            "renderer_stage_age_ms" in media_header and
            "videoRenderStageName(media_health.renderer_stage)" in watchdog and
            "render_stage=%s" in watchdog and
            "render_stage_age_ms=%llu" in watchdog,
            "watchdog incidents must correlate RTP/decode/present health with the last renderer stage")
    require("prepareFreshSessionReconnect" in run_loop,
            "fresh reconnects must serialize transport and media teardown")

    print("Xbox stream session order tests passed")


if __name__ == "__main__":
    main()
