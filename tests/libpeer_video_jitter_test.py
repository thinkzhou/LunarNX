#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    desktop_make = Path("Makefile.desktop").read_text()
    switch_make = Path("Makefile.switch").read_text()
    jitter_header = Path("src/webrtc/video_rtp_jitter_buffer.h").read_text()
    estimator = Path("src/webrtc/network_path_estimator.h").read_text()
    policy = Path("src/webrtc/video_jitter_policy.h").read_text()
    peer_manager = Path("src/webrtc/peer_manager.cpp").read_text()
    peer_header = Path("src/webrtc/peer_manager.h").read_text()
    session = Path("src/app/xbox_stream_session.cpp").read_text()
    media = Path("src/stream/media_pipeline.h").read_text()
    patch = Path(
        "tools/libpeer_legacy/legacy-libpeer-switch.patch"
    ).read_text()

    require(
        "src/webrtc/video_rtp_jitter_buffer.cpp" in desktop_make
        and "src/webrtc/video_rtp_jitter_buffer.cpp" in switch_make,
        "desktop and Switch builds must compile the video jitter buffer",
    )
    require(
        desktop_make.count("$(BUILD)/src/webrtc/video_rtp_jitter_buffer.o") >= 3,
        "desktop app and both PeerManager probes must link the jitter buffer",
    )
    require(
        "config.raw_video_rtp = 1" in peer_manager
        and "video_jitter_.receive" in peer_manager,
        "PeerManager must receive raw video RTP through the reorder buffer",
    )
    require(
        "peer_connection_send_nack" in peer_manager
        and "peer_connection_send_receiver_feedback_stats" in peer_manager,
        "PeerManager must send NACK and report jitter-buffer loss accounting",
    )
    require(
        peer_manager.count("peer_connection_send_rtcp_pil") == 1
        and "jitter recovery RTCP PLI" not in peer_manager,
        "Jitter recovery must coalesce PLI through the session request path",
    )
    require(
        "NetworkPathEstimator" in peer_header
        and "computeVideoJitterPolicy" in peer_manager
        and "setMissingPacketHoldMs" in peer_manager
        and "NetworkPathMode::Home" in estimator
        and "enum class NetworkPathMode" in estimator
        and "Cloud," in estimator
        and "rtt_inflation_ms" in estimator
        and "missing_packet_hold_ms" in policy
        and "max_head_blocked_frames = 3" in policy
        and "max_head_blocked_frames = 6" in policy
        and "setHeadBlockedPolicy" in jitter_header
        and "kMediaStatsCacheInterval{250}" in peer_manager
        and "networkStatsSnapshot" in peer_manager
        and "setVideoJitterMode" in peer_manager,
        "Adaptive jitter must use the shared path estimate and profile-specific bounds",
    )
    jitter_source = Path("src/webrtc/video_rtp_jitter_buffer.cpp").read_text()
    require(
        "missingPacketHoldMs" in jitter_source
        and "missing_packets_unrecovered" in jitter_source
        and "countUnrecovered" in jitter_source,
        "jitter buffer must apply the policy deadline and count final loss",
    )
    require(
        "enum class NetworkPathQuality" in estimator
        and "NetworkPathQuality::Good" in estimator
        and "NetworkPathQuality::Fair" in estimator
        and "NetworkPathQuality::Poor" in estimator,
        "shared path estimation must expose quality tiers",
    )
    require(
        "updateNetworkPathEstimate(network_stats)" in peer_manager
        and "bad_windows_ >= 2" in estimator
        and "good_windows_ >= 3" in estimator
        and "xbox-net-quality" in peer_manager,
        "network path quality must use rolling evidence, hysteresis, and telemetry",
    )
    require(
        "on_video_recovery" in peer_header
        and "media_.requestVideoRecovery" in session
        and "requestVideoRecovery" in media,
        "RTP damage must request a fresh IDR without flushing fenced frames",
    )
    require(
        "kMaxBufferedFrames = 32" in jitter_header
        and "kMaxBufferedPackets = 2048" in jitter_header
        and "kMaxBufferedBytes = 3 * 1024 * 1024" in jitter_header
        and "kMaxAccessUnitBytes = 2 * 1024 * 1024" in jitter_header,
        "jitter-buffer heap usage must remain protected by hard limits",
    )
    require(
        "raw_video_rtp" in patch
        and "peer_connection_send_nack" in patch
        and "peer_connection_send_receiver_feedback_stats" in patch,
        "tracked legacy libpeer patch must reproduce the active RTP/NACK path",
    )

    print("libpeer video jitter integration tests passed")


if __name__ == "__main__":
    main()
