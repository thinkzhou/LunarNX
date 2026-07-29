#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    peer_source = (ROOT / "lib/libpeer/src/peer_connection.c").read_text()
    peer_header = (ROOT / "lib/libpeer/src/peer_connection.h").read_text()
    srtp = (ROOT / "lib/libpeer/src/dtls_srtp.c").read_text()
    socket = (ROOT / "lib/libpeer/src/socket.c").read_text()
    socket_header = (ROOT / "lib/libpeer/src/socket.h").read_text()
    peer_manager = (ROOT / "src/webrtc/peer_manager.cpp").read_text()
    session = (ROOT / "src/app/xbox_stream_session.cpp").read_text()
    tracked_patch = (
        ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch"
    ).read_text()

    require("remote_policy.window_size" in srtp and "1024" in srtp,
            "Inbound SRTP replay window must retain delayed Xbox retransmissions")
    for field in (
        "srtp_rtp_auth_failures",
        "srtp_rtp_replay_failures",
        "srtp_rtp_replay_old_failures",
        "srtp_rtp_other_failures",
    ):
        require(field in peer_source and field in peer_header,
                f"SRTP diagnostics must expose {field}")
        require(field in tracked_patch,
                f"Tracked legacy patch must preserve {field}")

    require("rtp_queue_depth" in peer_header and
            "rtp_queue_oldest_age_ms" in peer_header,
            "Media stats must expose current RTP queue pressure and age")
    require("enqueued_at_ms" in peer_source,
            "Queued RTP packets must retain enqueue time for age diagnostics")
    require("receive_buffer_size" in socket_header and
            "getsockopt" in socket and "SO_RCVBUF" in socket,
            "UDP diagnostics must report the effective receive buffer")
    require("udp_receive_buffer_bytes" in peer_header,
            "Media stats must expose the effective UDP receive buffer")

    for field in (
        "pump_socket_receive_us_total",
        "pump_receive_loop_us_total",
        "pump_rtp_drain_us_total",
        "pump_socket_packets_total",
        "pump_rtp_packets_decoded_total",
    ):
        require(field in peer_source and field in peer_header,
                f"Pump phase diagnostics must expose {field}")
        require(field in tracked_patch,
                f"Tracked legacy patch must preserve {field}")
    require("CLOCK_MONOTONIC" in peer_source,
            "Pump phase diagnostics must use a monotonic microsecond clock")
    require("DEBUG-pump-phase" in peer_manager and
            "socket_us=" in peer_manager and
            "receive_loop_us=" in peer_manager and
            "rtp_drain_us=" in peer_manager and
            "outbound_us=" in peer_manager and
            "other_us=" in peer_manager,
            "Slow pump logs must split receive, RTP drain, outbound, and unaccounted time")

    for field in (
        "ice_local_candidate_type",
        "ice_remote_candidate_type",
        "ice_local_address",
        "ice_remote_address",
    ):
        require(field in peer_header,
                f"Selected ICE pair diagnostics must expose {field}")
    require("ice_pair=" in session and "srtp_detail=" in session and
            "rtp_queue_depth=" in session,
            "Session performance logs must include route, SRTP cause, and current queue pressure")

    print("libpeer stream recovery observability tests passed")


if __name__ == "__main__":
    main()
