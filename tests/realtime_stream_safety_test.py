#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def body(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def main() -> None:
    diagnostics = (ROOT / "src/diagnostics.h").read_text()
    peer_header = (ROOT / "src/webrtc/peer_manager.h").read_text()
    peer_source = (ROOT / "src/webrtc/peer_manager.cpp").read_text()
    channels = (ROOT / "src/app/xbox_channel_manager.cpp").read_text()
    dtls = (ROOT / "lib/libpeer/src/dtls_srtp.c").read_text()
    connection = (ROOT / "lib/libpeer/src/peer_connection.c").read_text()
    legacy_patch = (
        ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch"
    ).read_text()
    switch_wrapper = (ROOT / "src/platform/switch_wrapper.c").read_text()

    drop_log = body(diagnostics, "inline void dropDiagnosticLog(", "} // namespace lunar")
    require("enqueueDropDiagnostic" in drop_log,
            "drop diagnostics must enqueue instead of writing on the media thread")
    require("std::fopen" not in drop_log and "std::fclose" not in drop_log,
            "dropDiagnosticLog must not perform synchronous SD I/O")
    require("kDropDiagnosticQueueCapacity" in diagnostics and
            "dropDiagnosticQueueDrops" in diagnostics,
            "the async diagnostic queue must be bounded and count overflow")

    require("OutboundCommand" in peer_header and
            "outbound_commands_" in peer_header and
            "drainOutboundCommands" in peer_source,
            "all WebRTC sends must go through the owner-thread command queue")
    require("completeOutboundCommand" in peer_source and
            "peer_connection_is_transient_send_error" in peer_source and
            "reliable_send_failed_" in peer_header,
            "reliable commands must survive transient DTLS backpressure and expose fatal failure")
    require("kMaxOutboundCommands = 64" in peer_header and
            "kMaxOutboundPayloadBytes = 1024" in peer_header,
            "the outbound command count and per-message heap use must stay bounded")
    require("flushReliableData" in channels and
            "hasPendingReliableData" in channels and
            "consumeReliableSendFailure" in channels,
            "startup must wait for reliable commands to leave the owner queue")
    require("dropDiagnosticLog" in peer_source and
            '"webrtc-outbound"' in peer_source,
            "outbound drops must remain visible with APP_DIAG disabled")
    video_callback = body(peer_source, "void PeerManager::onVideoTrack(",
                          "void PeerManager::handleVideoJitterRecovery")
    require("enqueueNack" in video_callback and
            "peer_connection_send_nack" not in video_callback,
            "RTP callbacks must not synchronously send NACK packets")

    require("DTLS_SRTP_MAX_WRITE_ATTEMPTS" in dtls and
            "while (ret == MBEDTLS_ERR_SSL_WANT_READ" not in dtls,
            "DTLS writes must not spin without a bound")
    require("PEER_CONNECTION_LOOP_BUDGET_MS" in connection and
            "peer_loop_budget_expired" in connection,
            "the libpeer receive/decode loop must enforce an inner deadline")
    require("peer_connection_drain_pending_dtls" in connection and
            connection.index("peer_connection_drain_pending_dtls(\n          pc") <
            connection.index("agent_recv(&pc->agent", connection.index("case PEER_CONNECTION_COMPLETED")),
            "pending DTLS records must resume before waiting for another datagram")
    for marker in (
        "DTLS_SRTP_MAX_WRITE_ATTEMPTS",
        "PEER_CONNECTION_LOOP_BUDGET_MS",
        "peer_loop_budget_expired",
        "peer_connection_is_transient_send_error",
        "peer_connection_drain_pending_dtls",
    ):
        require(marker in legacy_patch,
                f"the reproducible legacy libpeer patch must include: {marker}")

    require("cfg.udp_rx_buf_size = 512 * 1024" in switch_wrapper,
            "the approved 512 KiB Switch UDP receive pool must remain unchanged")

    print("Realtime stream safety tests passed")


if __name__ == "__main__":
    main()
