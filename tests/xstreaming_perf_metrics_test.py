#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    reference = (ROOT / "github_repos/XStreaming/src/components/PerfPanel.tsx").read_text()
    perf_stats = (ROOT / "src/stream/perf_stats.h").read_text()
    overlay = (ROOT / "src/ui/stream_overlay.cpp").read_text()
    overlay_header = (ROOT / "src/ui/stream_overlay.h").read_text()
    detail_overlay = (ROOT / "src/ui/perf_overlay.cpp").read_text()
    stream_view = (ROOT / "src/ui/stream_view.cpp").read_text()
    media_pipeline = (ROOT / "src/stream/media_pipeline.cpp").read_text()
    peer_header = (ROOT / "lib/libpeer/src/peer_connection.h").read_text()
    peer_source = (ROOT / "lib/libpeer/src/peer_connection.c").read_text()
    agent_header = (ROOT / "lib/libpeer/src/agent.h").read_text()
    agent_source = (ROOT / "lib/libpeer/src/agent.c").read_text()
    legacy_patch = (ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch").read_text()

    for metric in ("RTT", "JIT", "FPS", "FD", "PL", "Bitrate", "DT"):
        require(f"t('{metric}')" in reference,
                f"XStreaming reference must expose {metric}")

    for field in (
        "network_rtt_ms",
        "video_jitter_us",
        "encoded_video_bytes",
        "video_frame_drops",
    ):
        require(field in perf_stats, f"LunarNX PerfStats must expose {field}")

    require("ice_rtt_ms" in peer_header and "ice_rtt_ms" in legacy_patch,
            "legacy libpeer must expose a reproducible ICE RTT sample")
    require("agent_send_consent_check" in agent_header and
            "agent_send_consent_check(&pc->agent)" in peer_source and
            "consent_check_pending" in agent_source and
            "AGENT_CONSENT_INTERVAL_MS" in agent_source,
            "streaming RTT must be refreshed by periodic ICE consent checks")
    require('"lunarnx/perf/hud_metrics"' in overlay and
            "metrics_label_->setText(brls::getStr(" in overlay,
            "single-row HUD metrics must use the active locale catalog")
    for key in (
        "detail_frames",
        "detail_decode",
        "detail_render",
        "detail_filters",
        "detail_audio",
        "detail_access_units",
        "detail_rtp",
        "detail_resolution",
        "detail_backend",
        "detail_packet_loss",
    ):
        require(f'"lunarnx/perf/{key}"' in detail_overlay,
                f"detailed diagnostics must localize {key}")
    require("rtp_video_missing_packets.load()" in detail_overlay and
            "ps_frames_lost" in detail_overlay and
            "has_ps_transport" in detail_overlay,
            "detailed overlay must keep Xbox RTP loss and PS chiaki loss paths separate")

    require("last_encoded_bytes_" in overlay_header and
            "last_decode_total_us_" in overlay_header and
            "decode_total_ns" in perf_stats,
            "bitrate and decode time must use interval deltas instead of lifetime averages")
    require("alignas(64) std::atomic<uint64_t> decode_total_us" in perf_stats,
            "decoder counters must not share a cache line with Xbox RTP hot-path stats")
    require("getStreamWidth()" in stream_view and "getStreamHeight()" in stream_view,
            "HUD resolution must include actual width and height")
    require("recordVideoQueueDrops(" in media_pipeline and
            "recordVideoSyncDrop()" in media_pipeline and
            "VideoSyncAction::Drop" in media_pipeline,
            "frame-drop stats must include queue pressure and AV-sync drops")

    print("XStreaming performance metric tests passed")


if __name__ == "__main__":
    main()
