#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    controller = Path("src/ps/ps_stream_controller.cpp").read_text()
    session = Path("src/ps/ps_stream_session.cpp").read_text()
    decoder = Path("src/stream/video_decoder.cpp").read_text()
    pipeline = Path("src/stream/media_pipeline.cpp").read_text()
    replay = Path("src/ps/ps_mock_replay_session.cpp").read_text()
    fixture = Path("tools/ps_media_replay/generate_fixture.sh").read_text()

    require("console.target >= 1000000" in controller and
            "stream::VideoCodec::H264" in controller,
            "PS4 must force H.264 even when HEVC is selected")
    require("CHIAKI_CODEC_H265" in session and "CHIAKI_CODEC_H264" in session,
            "PS session must select the requested Chiaki codec")
    require("AV_CODEC_ID_HEVC" in decoder and "AV_CODEC_ID_H264" in decoder,
            "shared decoder must initialize both H.264 and HEVC")
    require("inspectVideoAccessUnit(video_codec_" in pipeline and
            "video_path_ == VideoPipelinePath::Xbox" in pipeline,
            "queue recovery must select Xbox or PS random-access detection")
    require("VideoPipelinePath::PlayStation" in controller,
            "PS media must explicitly select the PlayStation decoder path")
    require('"hevc_mp4toannexb"' in replay,
            "controller replay must convert HEVC MP4 packets to Annex-B")
    require("PS_MEDIA_REPLAY_CODEC" in fixture and "libx265" in fixture,
            "fixture generator must produce deterministic HEVC input")
    print("PS HEVC support tests passed")


if __name__ == "__main__":
    main()
