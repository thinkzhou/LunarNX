#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    diagnostics = (ROOT / "src/diagnostics.h").read_text()
    perf = (ROOT / "src/stream/perf_stats.h").read_text()
    session = (ROOT / "src/app/xbox_stream_session.cpp").read_text()
    peer = (ROOT / "src/webrtc/peer_manager.cpp").read_text()
    jitter = (ROOT / "src/webrtc/video_rtp_jitter_buffer.cpp").read_text()
    renderer = (ROOT / "src/stream/video_renderer.cpp").read_text()
    switch_makefile = (ROOT / "Makefile.switch").read_text()

    require("LATENCY_DIAG ?= 0" in switch_makefile,
            "latency tracing must remain opt-in")
    require("LUNARNX_LATENCY_DIAGNOSTIC_LOG=$(LATENCY_DIAG)" in switch_makefile,
            "Switch builds must expose the latency diagnostic flag")
    require("APP_DIAG ?= 0" in switch_makefile,
            "the diagnostic build must not require synchronous APP_DIAG logging")
    require("enqueueDropDiagnostic(line.data(), used, false)" in diagnostics,
            "latency summaries must use the bounded asynchronous writer")
    for field in ("write_total_us", "write_max_us", "queue_high_watermark",
                  "file_opens", "flushes", "dropped"):
        require(field in diagnostics,
                f"the writer must report self-interference field: {field}")
    for component in ('"network"', '"video-audio"', '"input-pump"',
                      '"logger"'):
        require(component in session,
                f"the one-second trace must include {component}")
    require("assembly_total_us" in jitter,
            "RTP frame assembly residence must be measured")
    for metric in ("input_queue_total_us", "latency_pump_total_us_",
                   "takeLatencyWindow"):
        require(metric in peer,
                f"WebRTC latency tracing must include {metric}")
    for metric in ("latency_decode_total_us", "latency_render_queue_total_us",
                   "latency_present_fence_total_us",
                   "latency_input_snapshot_age_total_us",
                   "latency_renderer_enqueue_total_us"):
        require(metric in perf,
                f"pipeline latency tracing must include {metric}")
    require("pending_frame_queued_ns" in renderer and
            "recordRenderQueueWait" in renderer,
            "decoded-frame queue residence must be measured")
    require("stalePresentationFramesToDrop" in renderer,
            "latest-frame presentation must discard stale decoded frames")
    require("#if LUNARNX_DROP_DIAGNOSTIC_LOG || LUNARNX_LATENCY_DIAGNOSTIC_LOG"
            in renderer,
            "GPU fence timing must be enabled by the latency build")

    print("Latency diagnostics contract tests passed")


if __name__ == "__main__":
    main()
