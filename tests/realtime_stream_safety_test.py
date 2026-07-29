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
    renderer_header = (ROOT / "src/stream/video_renderer.h").read_text()
    renderer = (ROOT / "src/stream/video_renderer.cpp").read_text()
    legacy_patch = (
        ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch"
    ).read_text()
    switch_wrapper = (ROOT / "src/platform/switch_wrapper.c").read_text()

    drop_format = body(diagnostics,
                       "inline void enqueueFormattedDropDiagnostic(",
                       "inline void dropDiagnosticLog(")
    drop_log = body(diagnostics,
                    "inline void dropDiagnosticLog(",
                    "inline void cloud1080CrashProbeLog(")
    require("enqueueDropDiagnostic" in drop_format and
            "enqueueFormattedDropDiagnostic" in drop_log,
            "drop diagnostics must enqueue through the async formatter")
    require("std::fopen" not in drop_format + drop_log and
            "std::fclose" not in drop_format + drop_log,
            "dropDiagnosticLog must not perform synchronous SD I/O")
    crash_probe_log = body(diagnostics,
                           "inline void cloud1080CrashProbeLog(",
                           "} // namespace lunar")
    require("enqueueFormattedDropDiagnostic" in crash_probe_log and
            "std::fopen" not in crash_probe_log and
            "std::fclose" not in crash_probe_log,
            "cloud 1080p crash probes must not perform synchronous SD I/O")
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
    require("selectOutboundCommand" in peer_source and
            "isSctpCommand" in peer_source and
            "allow_sctp = false" in peer_source,
            "RTCP recovery traffic must bypass transient SCTP backpressure")
    enqueue_nack = body(peer_source, "bool PeerManager::enqueueNack(",
                        "bool PeerManager::sendInputData")
    require("waitingForKeyframe" not in enqueue_nack and
            "milliseconds(250)" in enqueue_nack,
            "Current recovery-frame NACKs must be queued with a short lifetime")
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
    require("kMaxDrainPasses = 8" in peer_source and
            "kMaxDrainTime = std::chrono::milliseconds(3)" in peer_source,
            "the owner-thread WebRTC pump must retain its outer work budget")
    for marker in (
        "PEER_CONNECTION_LOOP_BUDGET_MS",
        "PEER_CONNECTION_RECEIVE_BUDGET_MS",
        "PEER_CONNECTION_RTP_PRE_DECODE_BUDGET",
        "PEER_CONNECTION_MAX_DTLS_PENDING_READS",
        "peer_loop_budget_expired",
        "peer_connection_drain_pending_dtls",
    ):
        require(marker not in connection,
                f"libpeer must match main without an inner work limit: {marker}")
        require(marker not in legacy_patch,
                f"the reproducible patch must omit the inner work limit: {marker}")
    completed = connection[connection.index("case PEER_CONNECTION_COMPLETED"):]
    require("while (dtls_srtp_has_pending(&pc->dtls_srtp))" in completed,
            "pending DTLS records must be fully drained like main")
    for marker in (
        "DTLS_SRTP_MAX_WRITE_ATTEMPTS",
        "peer_connection_is_transient_send_error",
    ):
        require(marker in legacy_patch,
                f"the reproducible legacy libpeer patch must include: {marker}")

    require("cfg.udp_rx_buf_size = 512 * 1024" in switch_wrapper,
            "the approved 512 KiB Switch UDP receive pool must remain unchanged")

    require("bool prepareDecoderReset();" in renderer_header,
            "Decoder reset must report whether the GPU handoff completed")
    prepare_reset = body(renderer, "bool VideoRenderer::prepareDecoderReset()",
                         "bool VideoRenderer::pollEvents()")
    present = body(renderer, "void VideoRenderer::present()",
                   "bool VideoRenderer::prepareDecoderReset()")
    require("wait_for" in prepare_reset and "waitIdle" not in prepare_reset,
            "The video worker must wait for, but never operate, the UI-owned GPU queue")
    require("decoder_reset_requested" in present and
            "present_ring->begin" in present and
            "decoder_reset_drain_steps" in present and
            "waitIdle()" not in present and
            "decoder_reset_ready" in present,
            "UI present must retire fenced NVDEC work without waiting the Borealis queue")

    print("Realtime stream safety tests passed")


if __name__ == "__main__":
    main()
