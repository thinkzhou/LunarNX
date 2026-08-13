#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


decoder = (ROOT / "src/stream/video_decoder.cpp").read_text()
controller = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
renderer = (ROOT / "src/stream/video_renderer.cpp").read_text()

decode_start = decoder.index("bool VideoDecoder::decode(")
hardware_start = decoder.index("#ifdef __SWITCH__", decode_start)
decode_gate = decoder[decode_start:hardware_start]
require("if (!au.has_vcl)" in decode_gate and
        decode_gate.index("if (!au.has_vcl)") <
        decode_gate.index("if (!decoder_ready_)") and
        "parameter_sets_" in decode_gate,
        "parameter-set/metadata-only video AUs must be consumed before the "
        "decoder gate, cached, and never submitted to NVDEC")
require("parameter_sets_pending_" in decoder and
        "startup_access_unit" in decoder,
        "cached codec parameter sets must be prepended to the first VCL access unit")

require("last_recovery_request_" in controller and
        "requestRecoveryIDR" in controller and
        "std::chrono::seconds(1)" in controller,
        "first-frame recovery IDR requests must be throttled")

reset_start = renderer.index("bool VideoRenderer::prepareDecoderReset()")
reset_end = renderer.index("bool VideoRenderer::pollEvents()", reset_start)
reset = renderer[reset_start:reset_end]
require("submitted_frames" in reset and
        "!s->pending_frame" in reset and
        "!s->current_frame" in reset and
        "return true" in reset,
        "hardware decoder reset before the first frame must not wait for a UI "
        "GPU handoff that cannot occur")

print("PS H.264 startup gate tests passed")
