#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("src/tools/ps_media_replay.cpp").read_text()
    makefile = Path("Makefile.switch.psmedia").read_text()
    fixture = Path("tools/ps_media_replay/generate_fixture.sh").read_text()

    require('av_bsf_get_by_name("h264_mp4toannexb")' in source,
            "MP4 H.264 must be converted to Chiaki-compatible Annex-B")
    require("bridge_->onVideoSample" in source,
            "video access units must enter the production PS bridge")
    require("bsf->par_out->extradata" not in source and
            "reason=video_profile_header" not in source,
            "replay must not invent a separate profile-header callback")
    require("bridge_->audioSink()" in source and
            "sink.header_cb(&header" in source and
            "sink.frame_cb(packet->data" in source,
            "Opus header and packets must enter the production PS bridge")
    require("sink.frame_cb(nullptr, 0" in source,
            "replay must exercise Chiaki's Opus PLC contract")
    require("MediaPipeline" in source and "kVideoBackend" in source and
            "presentVideoFrame" in source,
            "replay must use the production Switch decode/render pipeline")
    require("SoftwareVideoFrameSink::instance().snapshot()" in source and
            "usesZeroCopyRender(kVideoBackend)" in source,
            "replay view must match production copy-out/zero-copy presentation")
    require("automatic_plc_done" in source and "automatic_loss_done" in source and
            "automatic_restart_done" in source,
            "replay must exercise PLC, loss, and restart without user input")
    require("PSMEDIA_BACKEND ?= software" in makefile and
            "LUNARNX_PSMEDIA_BACKEND" in makefile,
            "replay must support differential software/copyout/zero-copy builds")
    require("build/switch-psmedia-$(PSMEDIA_BACKEND)" in makefile,
            "backend builds must not reuse objects compiled for another mode")
    require("APP_DIAG ?= 1" in makefile,
            "replay must retain diagnostics when the emulator aborts")
    require("ps_media_bridge.cpp" in makefile and
            "media_pipeline.cpp" in makefile and
            "include Makefile.switch" in makefile,
            "standalone NRO must link the production PS media components")
    require("-frame_duration 10" in fixture and "-ar 48000 -ac 2" in fixture,
            "fixture audio must match the observed PS Opus stream")
    require('PS_MEDIA_REPLAY_VIDEO_SOURCE' in fixture and
            '-c:v copy' in fixture and '-stream_loop -1' in fixture,
            "fixture generator must support the known-good Xbox H.264 track")
    require("-b:v 2200k" in fixture and "-maxrate 3000k" in fixture,
            "fixture bitrate must remain representative of captured PS media")

    print("PS media replay tests passed")


if __name__ == "__main__":
    main()
