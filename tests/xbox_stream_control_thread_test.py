#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def main() -> None:
    header = (ROOT / "src/app/xbox_stream_session.h").read_text()
    source = (ROOT / "src/app/xbox_stream_session.cpp").read_text()

    require("std::thread control_thread_;" in header,
            "XboxStreamSession must own a joinable control-plane thread")
    require("std::thread input_thread_;" in header and
            "std::atomic<bool> input_loop_stop_" in header,
            "XboxStreamSession must own a cancellable input producer thread")
    require("std::condition_variable control_cv_;" in header,
            "the control-plane wait must be interruptible during shutdown")
    require("std::mutex session_api_mutex_;" in header,
            "reconnect signaling and control HTTP must not race XboxApiClient state")
    require("void controlLoop(" in header,
            "keep-alive and token refresh need a dedicated control loop")

    run_loop = function_body(
        source,
        "void XboxStreamSession::runLoop(",
        "void XboxStreamSession::controlLoop(",
    )
    control_loop = function_body(
        source,
        "void XboxStreamSession::controlLoop(",
        "void XboxStreamSession::cleanupResources(",
    )
    stop = function_body(
        source,
        "void XboxStreamSession::stop(",
        "std::string XboxStreamSession::sessionId()",
    )
    start = function_body(
        source,
        "bool XboxStreamSession::start(",
        "void XboxStreamSession::stop(",
    )

    require("session_client_.keepAlive" not in run_loop,
            "the media/WebRTC pump loop must never perform synchronous keep-alive HTTP")
    require("refresh_tokens" not in run_loop,
            "the media/WebRTC pump loop must never perform synchronous token refresh")
    require("session_client_.keepAliveDetailed" in control_loop,
            "the control loop must own keep-alive HTTP")
    require("callbacks.refresh_tokens" in control_loop,
            "the control loop must serialize token refresh with keep-alive")
    require(control_loop.index("session_client_.keepAliveDetailed") <
            control_loop.index("callbacks.refresh_tokens"),
            "preserve the original keep-alive-before-token-refresh order")
    require("std::chrono::seconds(std::max(1, keep_alive_seconds))" in control_loop,
            "keep-alive cadence must use the server-provided interval")
    require("keep_alive_result.isAuthError()" in control_loop and
            "callbacks.refresh_tokens(true, cancel)" in control_loop and
            control_loop.count("keepAliveDetailed(") >= 2,
            "401/403 keep-alive failures must force token refresh and retry once")
    require("preserving WebRTC media" in control_loop and
            "transport_.disconnect()" not in control_loop,
            "keep-alive failures must not directly tear down healthy WebRTC")
    require('"token refresh failed action=preserve-media"' in control_loop and
            '"token refresh failed action=reconnect"' not in control_loop and
            "control_recovery_requested_ = true" not in
            control_loop[control_loop.index("if (!refresh_ok)"):],
            "periodic token refresh failures must preserve healthy WebRTC")
    require("next_keep_alive = now + keep_alive_interval;" in control_loop,
            "keep-alive cadence must remain anchored to its pre-request time")
    require("next_token_refresh = now + token_refresh_interval;" in control_loop,
            "token refresh cadence must remain anchored to its pre-request time")
    require(control_loop.count("api_lock(session_api_mutex_)") >= 2,
            "token refresh and keep-alive must exclude reconnect signaling")
    require("control_cv_.wait_until" in control_loop,
            "the control loop must wait without polling")
    require("!streaming_.load() || isCancelled(callbacks)" in control_loop,
            "keep-alive cancellation must observe both stream exit and owner shutdown")

    notify_pos = stop.index("control_cv_.notify_all()")
    stream_join_pos = stop.index("stream_thread_to_join.join()")
    control_join_pos = stop.index("control_thread_to_join.join()")
    input_join_pos = stop.index("input_thread_to_join.join()")
    cleanup_pos = stop.index("cleanupResources(delete_session)")
    require(notify_pos < stream_join_pos < cleanup_pos,
            "shutdown must wake and join the media thread before cleanup")
    require(notify_pos < control_join_pos < cleanup_pos,
            "shutdown must wake and join the control thread before cleanup")
    require(notify_pos < input_join_pos < cleanup_pos,
            "shutdown must join the input producer before input resources are cleaned up")
    require(".detach()" not in stop,
            "owned stream threads must never outlive XboxStreamSession")

    require(start.index("startInputLoop(callbacks)") <
            start.index("stream_thread_ = std::thread"),
            "the input producer must start before the WebRTC owner loop")
    require(start.index("stream_thread_ = std::thread") <
            start.index("while (!media_startup_ready_.load()") <
            start.index("callbacks.on_streaming()"),
            "the stream page must open only after the owner loop enables media")

    require("reconnectWithFreshSession" in run_loop,
            "media failure should rebuild a fresh Xbox session")
    require("media_.getHealthStats()" in run_loop and
            "video watchdog" in run_loop and
            "kMediaHealthPollInterval{50}" in source and
            "health_poll_due" in run_loop,
            "the owner loop must monitor RTP, decode, and present liveness")
    reconnect_section = function_body(
        source,
        "bool XboxStreamSession::reconnectWithFreshSession(",
        "webrtc::PeerCallbacks XboxStreamSession::createPeerCallbacks(",
    )
    reconnect_prepare_section = function_body(
        source,
        "void XboxStreamSession::prepareFreshSessionReconnect(",
        "bool XboxStreamSession::reconnectWithFreshSession(",
    )
    require("api_lock(session_api_mutex_)" in reconnect_section,
            "reconnect signaling must exclude control-plane API operations")
    require("prepareFreshSessionReconnect" in reconnect_section and
            "media_.prepareForNewMediaSource" in reconnect_prepare_section,
            "a fresh WebRTC association must reset the complete media source epoch")

    print("Xbox stream control thread tests passed")


if __name__ == "__main__":
    main()
