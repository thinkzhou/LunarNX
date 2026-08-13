#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    decoder = Path("src/stream/video_decoder.cpp").read_text()
    renderer = Path("src/stream/video_renderer.cpp").read_text()

    require("video decode begin" in decoder,
            "Video decoder should log packet entry for stream-layer diagnosis")
    require("avcodec_send_packet ret" in decoder,
            "Video decoder should log FFmpeg packet submission results")
    require("avcodec_receive_frame frame" in decoder,
            "Video decoder should log decoded AVFrame output")
    require("video decode done" in decoder,
            "Video decoder should log per-packet decoded frame counts")
    require("AV_CODEC_ID_HEVC" in decoder and "av_parser_init(codec_id)" in decoder,
            "Video decoder should select the FFmpeg parser for H.264 or HEVC")
    require("PARSER_FLAG_COMPLETE_FRAMES" in decoder,
            "Video decoder should treat RTP-depacketized access units as complete H.264 frames")
    require("decoder gate opened" in decoder and
            "until parameter sets/random access" in decoder,
            "Video decoder should wait for codec parameter sets and random access")
    require("kVideoErrorLogLimit" in decoder and
            "error_log_count_" in decoder,
            "Video decoder FFmpeg errors should be rate-limited")
    require("NVDEC doesn't need a parser" not in decoder and
            "feed raw NAL units directly" not in decoder,
            "Switch NVDEC must not receive arbitrary RTP-reassembled NAL units directly")

    require("#include \"../diagnostics.h\"" in renderer,
            "Video renderer should write diagnostics to the same app log")
    require("render success" in renderer,
            "Video renderer should log successful NVTEGRA frame mapping")
    require("present submit" in renderer,
            "Video renderer should log successful present submissions")
    require("render reject" in renderer,
            "Video renderer should log why frames are rejected")

    print("Video pipeline logging tests passed")


if __name__ == "__main__":
    main()
