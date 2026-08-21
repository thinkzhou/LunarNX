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


codec = (ROOT / "src/stream/video_codec.h").read_text()
header = (ROOT / "src/stream/video_decoder.h").read_text()
decoder = (ROOT / "src/stream/video_decoder.cpp").read_text()
provider = (ROOT / "src/stream/stream_backend_provider.cpp").read_text()
pipeline = (ROOT / "src/stream/media_pipeline.h").read_text()
media = (ROOT / "src/stream/media_pipeline.cpp").read_text()

require("class XboxVideoDecoder : public VideoDecoder" in header,
        "Xbox must own a dedicated decoder class")
require("class PsVideoDecoder : public VideoDecoder" in header,
        "PlayStation must own a dedicated decoder class")
require("virtual VideoAccessUnitInfo inspectAccessUnit" in header,
        "decoder base must expose path-owned access-unit inspection")

# Shared base must not carry the transport path enum: policy belongs to the
# per-path subclasses.
base = header[header.index("class VideoDecoder"):header.index("class XboxVideoDecoder")]
require("VideoPipelinePath" not in base,
        "shared decoder base must not branch on the transport path")

xbox_inspect = method_body(
    decoder, "VideoAccessUnitInfo XboxVideoDecoder::inspectAccessUnit(")
require("inspectXboxH264AccessUnit" in xbox_inspect and
        "appendVideoNalType" not in xbox_inspect,
        "Xbox inspection must use the allocation-free H.264 fast path")

# Xbox decoder restores standalone SPS/PPS caching (the 0.2 regression fix):
# parameter-set-only access units must be consumed and cached before the
# decoder gate, never handed to NVDEC.
xbox_decode = method_body(decoder, "bool XboxVideoDecoder::decode(")
require("if (!au.has_vcl)" in xbox_decode and
        xbox_decode.index("if (!au.has_vcl)") <
        xbox_decode.index("if (!decoder_ready_)") and
        "parameter_sets_" in xbox_decode,
        "Xbox standalone parameter sets must be cached before the decoder gate")
require("parameter_sets_pending_" in xbox_decode and
        "startup_access_unit" in xbox_decode,
        "Xbox cached parameter sets must be prepended to the first VCL access unit")
require("parameter_sets_pending_ = !parameter_sets_.empty()" in
        method_body(decoder, "bool XboxVideoDecoder::resetForKeyframe()"),
        "Xbox decoder reset must retain cached parameter sets across recovery")
require("Xbox decoder path only supports H.264" in decoder,
        "Xbox decoder must enforce its H.264-only constraint")

require("if (!inspected_access_unit)" in xbox_decode and
        "inspectAccessUnit(data, len)" in xbox_decode,
        "decoder must reuse access-unit metadata supplied by the Xbox queue")

require("make_unique<XboxVideoDecoder>()" in provider,
        "backend provider must select XboxVideoDecoder for the Xbox path")
require("VideoPipelinePath::Xbox" in pipeline,
        "Xbox must remain the default media path")

require("&packet.access_unit" in media and
        "inspectAccessUnit(data, len)" in media,
        "Xbox queue must delegate one-time inspection to the path decoder and "
        "pass the result to decode")

print("Xbox video fast-path tests passed")
