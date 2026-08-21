#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    desktop_make = Path("Makefile.desktop").read_text()
    switch_make = Path("Makefile.switch").read_text()
    jitter_header = Path("src/webrtc/video_rtp_jitter_buffer.h").read_text()
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
        "VideoRtpJitterBuffer::kMinHoldMs" in peer_manager
        and "smoothed_rtt_ms_" in peer_manager
        and "kHomeMaxJitterHoldMs = 48" in peer_manager
        and "kCloudMaxJitterHoldMs = 180" in peer_manager
        and "kCloudRecoveryHoldMs = 300" in peer_manager
        and "smoothed_rtt_ms_ + 40" in peer_manager
        and "smoothed_rtt_ms_ + 30" in peer_manager
        and "smoothed_rtt_ms_ * 2" in peer_manager
        and "cloud ? 6 : 2" in peer_manager
        and "setHeadBlockedPolicy" in jitter_header
        and "kMediaStatsCacheInterval{250}" in peer_manager
        and "networkStatsSnapshot" in peer_manager
        and "last_rtt_sample_ms_" in peer_manager
        and "setVideoJitterMode" in peer_manager,
        "Adaptive jitter hold must use smoothed, profile-specific low-latency bounds",
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
