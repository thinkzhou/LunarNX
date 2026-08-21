#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def method_body(source: str, signature: str) -> str:
    start = source.find(signature)
    require(start >= 0, f"Missing method: {signature}")
    brace = source.find("{", start)
    require(brace >= 0, f"Missing method body: {signature}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise SystemExit(f"FAIL: Unterminated method body: {signature}")


decoder_h = (ROOT / "src/stream/video_decoder.h").read_text()
decoder = (ROOT / "src/stream/video_decoder.cpp").read_text()
controller = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
renderer = (ROOT / "src/stream/video_renderer.cpp").read_text()

require("class PsVideoDecoder : public VideoDecoder" in decoder_h,
        "PlayStation must own a dedicated decoder class")

# Chiaki can deliver parameter sets as a standalone access unit. The PS
# decoder must consume and cache those before the decoder gate and never
# submit them to NVDEC.
ps_decode = method_body(decoder, "bool PsVideoDecoder::decode(")
require("if (!au.has_vcl)" in ps_decode and
        ps_decode.index("if (!au.has_vcl)") <
        ps_decode.index("if (!decoder_ready_)") and
        "parameter_sets_" in ps_decode,
        "parameter-set/metadata-only video AUs must be consumed before the "
        "decoder gate, cached, and never submitted to NVDEC")
require("parameter_sets_pending_" in ps_decode and
        "startup_access_unit" in ps_decode,
        "cached codec parameter sets must be prepended to the first VCL access unit")
require("bool packet_accepted = false" in decoder and
        "if (!packet_accepted)" in decoder and
        "submitted_timestamps_.push_back(timestamp)" in decoder,
        "NVDEC decode must distinguish accepted packets from drained output and "
        "track timestamps only for accepted access units")
require("if (prepended_parameter_sets) parameter_sets_pending_ = false" in decoder and
        decoder.index("if (prepended_parameter_sets) parameter_sets_pending_ = false") >
        decoder.index("bool packet_accepted = false"),
        "cached parameter sets must remain pending until the AU is accepted")

require("last_recovery_request_" in controller and
        "requestRecoveryIDR" in controller and
        "std::chrono::seconds(1)" in controller,
        "first-frame recovery IDR requests must be throttled")

reset_start = renderer.index("bool VideoRenderer::prepareDecoderReset()")
reset_end = renderer.index("bool VideoRenderer::pollEvents()", reset_start)
reset = renderer[reset_start:reset_end]
require("submitted_frames" in reset and
        "s->pending_count==0" in reset and
        "!s->current_frame" in reset and
        "return true" in reset,
        "hardware decoder reset before the first frame must not wait for a UI "
        "GPU handoff that cannot occur")

print("PS H.264 startup gate tests passed")
