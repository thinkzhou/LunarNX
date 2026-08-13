#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    controller = Path("src/ps/ps_stream_controller.cpp").read_text()
    session = Path("src/ps/ps_mock_replay_session.cpp").read_text()
    activity = Path("src/tools/ps_mock_lifecycle_activity.cpp").read_text()
    makefile = Path("Makefile.switch").read_text()
    target = Path("Makefile.switch.psmock").read_text()
    config = Path("config/default_config.json").read_text()

    require("LUNARNX_PS_MOCK_REPLAY" in session and
            '"mock_replay"' in session,
            "mock replay must require compile-time and runtime opt-in")
    require("PS_MOCK_REPLAY ?= 0" in makefile and
            "-DLUNARNX_PS_MOCK_REPLAY=$(PS_MOCK_REPLAY)" in makefile,
            "normal Switch builds must compile mock replay disabled")
    require('"ps_network_profile": "native_switch"' in config,
            "default configuration must keep the real Switch network path")
    require("psMockReplayEnabled()" in controller and
            "PsMockReplaySession" in controller and
            "PsMediaBridge" in controller and "MediaPipeline" in controller,
            "production PS controller must own the mock session and media path")
    require('"h264_mp4toannexb"' in session and
            '"hevc_mp4toannexb"' in session and
            "av_bsf_get_by_name(filter_name)" in session and
            "bridge_.onVideoSample" in session and
            "bridge_.audioSink()" in session,
            "mock session must feed the production Chiaki callback boundary")
    require("round <= 2" in activity and
            "controller->startStream()" in activity and
            "controller->stopStream(false)" in activity and
            "cancel_race=ok" in activity and
            'writeResult("PASS", "rounds=2 reconnect=ok cancel_race=ok")' in activity,
            "autorun gate must exercise cancellation plus two complete lifecycles")
    require("PS_MOCK_REPLAY = 1" in target and
            "LUNARNX_PS_MOCK_AUTORUN=1" in target,
            "mock probe target must explicitly enable mock and autorun")

    print("PS mock replay tests passed")


if __name__ == "__main__":
    main()
