#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    diagnostics = (ROOT / "src/diagnostics.h").read_text()
    perf = (ROOT / "src/stream/perf_stats.h").read_text()
    pipeline = (ROOT / "src/stream/media_pipeline.cpp").read_text()
    decoder = (ROOT / "src/stream/video_decoder.cpp").read_text()
    renderer = (ROOT / "src/stream/video_renderer.cpp").read_text()
    av_sync = (ROOT / "src/stream/av_sync.cpp").read_text()
    jitter = (ROOT / "src/webrtc/video_rtp_jitter_buffer.cpp").read_text()
    switch_makefile = (ROOT / "Makefile.switch").read_text()

    require("APP_DIAG ?= 0" in switch_makefile,
            "normal per-frame diagnostics must remain disabled by default")
    require("DROP_DIAG ?= 1" in switch_makefile and
            "LUNARNX_DROP_DIAGNOSTIC_LOG=$(DROP_DIAG)" in switch_makefile,
            "sparse drop diagnostics must remain available in hardware builds")
    require("dropDiagnosticLog" in diagnostics and "[drop-diag t=" in diagnostics,
            "drop events need a searchable timestamped log prefix")
    require("logVideoDropDiagnostic" in perf,
            "drop sources must share one complete snapshot format")
    require('"rtp_loss"' in perf and
            '"video_missing_increased"' in perf and
            "video_missing > previous_video_missing" in perf,
            "video RTP loss increases must emit sparse drop diagnostics")
    require("kRtpLossEpisodeGapMs" in perf and
            "last_video_loss_observed_ms.exchange" in perf,
            "one RTP loss burst must not spam the SD-card log")
    for field in (
        "missing_delta", "video_gap_delta", "audio_missing_delta",
        "keepalive_interval_s", "keepalive_duration_ms", "keepalive_age_ms",
        "pump_gap_us", "pump_duration_us", "arrival_age_ms",
        "arrival_gap_ms", "highest_seq", "gap_packets", "ssrc_changes",
        "nacks", "resyncs", "jitter_q_packets", "waiting_keyframe",
        "rtp_queue_drop_delta", "keepalive_exception_count",
        "keepalive_exception_age_ms", "token_refresh_exception_count",
        "token_refresh_exception_age_ms",
    ):
        require(field in perf,
                f"RTP loss diagnostics must include correlation field: {field}")
    require("recordKeepAlive" in perf and "perf_.recordKeepAlive" in
            (ROOT / "src/app/xbox_stream_session.cpp").read_text(),
            "loss snapshots need the latest keepalive duration without logging every keepalive")
    session = (ROOT / "src/app/xbox_stream_session.cpp").read_text()
    require("recordKeepAliveException" in perf and
            "perf_.recordKeepAliveException" in session and
            "recordTokenRefreshException" in perf and
            "perf_.recordTokenRefreshException" in session,
            "control exceptions must be sampled in memory for the next drop snapshot")
    require("recordWebRtcPump" in perf and "last_webrtc_pump" in
            (ROOT / "src/app/xbox_stream_session.cpp").read_text(),
            "loss snapshots need WebRTC pump scheduling stalls")
    require("rtp-recovery" in perf and "recordRecoveryPli" in perf and
            '"idr_received"' in perf and '"displayed"' in perf,
            "loss episodes need a bounded PLI-to-IDR-to-display timeline")
    require("#if LUNARNX_DROP_DIAGNOSTIC_LOG" in perf and
            "#if LUNARNX_DROP_DIAGNOSTIC_LOG" in pipeline,
            "DROP_DIAG=0 must compile diagnostic sampling out of hot paths")
    jitter_log = jitter.index('lunar::dropDiagnosticLog(\n                    "rtp-jitter"')
    jitter_guard = jitter.rfind("#if LUNARNX_DROP_DIAGNOSTIC_LOG", 0, jitter_log)
    jitter_end = jitter.find("#endif", jitter_log)
    require(jitter_guard >= 0 and jitter_end > jitter_log,
            "jitter rejection details must compile only with DROP_DIAG enabled")
    for field in (
        "reject_reason", "hard_recovery", "frame_age_ms", "idle_age_ms",
        "marker_seen", "partition_head_seen", "contains_idr",
    ):
        require(field in jitter,
                f"jitter rejection diagnostics must include: {field}")
    for field in (
        "video_sync_drops", "video_queue_drops", "video_queue_packets",
        "video_queue_oldest_age_ms", "last_video_au_bytes",
        "last_video_au_queue_age_us", "last_decode_us",
        "decode_window_max_us", "last_present_wait_us",
        "present_wait_window_max_us", "last_av_raw_delay_ns",
        "last_av_audio_age_ms",
    ):
        require(field in perf, f"drop snapshot must contain {field}")

    for source in ("queue_overflow", "av_sync"):
        require(f'"{source}"' in pipeline,
                f"media pipeline must identify {source} drops")
    require("decode_error" in decoder and "logVideoDropDiagnostic" in decoder,
            "decoder errors must emit a drop snapshot")
    require("recordPresentWait" in renderer,
            "drop snapshots need the latest GPU command-ring wait")
    require("raw_delay_ns" in av_sync and "clamped_delay_ns" in av_sync,
            "A/V diagnostics must preserve delay before policy clamping")

    print("Drop diagnostics tests passed")


if __name__ == "__main__":
    main()
