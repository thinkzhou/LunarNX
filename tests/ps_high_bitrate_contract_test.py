#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
pipeline_h = (ROOT / "src/stream/media_pipeline.h").read_text()
pipeline = (ROOT / "src/stream/media_pipeline.cpp").read_text()
decoder = (ROOT / "src/stream/video_decoder.cpp").read_text()
recvbuf = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-recvbuf.patch").read_text()
adapter = (ROOT / "src/ps/chiaki_log_adapter.cpp").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require("AV_CODEC_FLAG_LOW_DELAY" in decoder and
        "AV_CODEC_FLAG2_FAST" in decoder and
        "ctx->has_b_frames = 0" in decoder and
        "ctx->thread_count = 1" in decoder and
        "ctx->extra_hw_frames = 16" in decoder,
        "Switch NVDEC must use the conservative low-latency configuration")
require("#define LUNARNX_PS_SKIP_LOOP_FILTER 0" in decoder and
        "#if LUNARNX_PS_SKIP_LOOP_FILTER" in decoder,
        "loop-filter skipping must remain an opt-in PS development switch")
for field in ("enqueued", "decoded", "intentional_drop", "alloc_fail",
              "depth_high", "bytes_high", "oldest_age_max_us",
              "enqueue_copy_total_us", "worker_decode_total_us",
              "recovery_overflow", "recovery_age", "recovery_decode",
              "idr_requests"):
    require(field in pipeline_h, f"PSQ metric {field} is missing")
require('"LUNARNX-PSQ"' in pipeline and
        "std::chrono::seconds(10)" in pipeline and
        "dropDiagnosticLog" in pipeline,
        "PSQ diagnostics must use ten-second asynchronous aggregation")
require(recvbuf.count("getsockopt(takion->sock, SOL_SOCKET, SO_RCVBUF") == 2,
        "local and PSN Takion sockets must both report actual receive buffers")
require("LUNARNX-PS-SOCKET rcvbuf_requested=%d rcvbuf_actual=%d" in recvbuf,
        "receive-buffer diagnostics must include requested and actual bytes")
require("LUNARNX-PS-SOCKET " in adapter,
        "socket aggregate diagnostics must avoid synchronous Chiaki-thread I/O")

print("PS high-bitrate diagnostics contract passed")
