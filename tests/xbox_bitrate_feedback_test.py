#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    profile = (ROOT / "src/app/stream_profile.h").read_text()
    channel = (ROOT / "src/app/xbox_channel_manager.cpp").read_text()
    session = (ROOT / "src/app/xbox_stream_session.cpp").read_text()
    peer = (ROOT / "lib/libpeer/src/peer_connection.c").read_text()
    peer_h = (ROOT / "lib/libpeer/src/peer_connection.h").read_text()
    sdp = (ROOT / "lib/libpeer/src/sdp.c").read_text()
    patch = (ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch").read_text()

    require("bitrate_kbps" in profile and "streamProfileBitrateKbps" in profile,
            "stream profiles must carry a resolution-aware receiver bitrate")
    require("clientdevicecapabilities" in channel and
            "maxBitrateKbps" in channel and
            "dimensionschanged" in channel,
            "message-channel startup must announce capabilities and dimensions")
    require("startProtocol(profile" in session,
            "startup protocol must receive the selected stream profile")
    require("max-fs=8160" in session and "max-mbps=489600" in session,
            "1080p profiles must expand the H.264 decode limits in the offer")
    require("peer_connection_send_receiver_feedback" in peer_h and
            "peer_connection_send_receiver_feedback" in peer,
            "libpeer must expose receiver feedback to the app")
    require("kReceiverFeedbackInterval{1}" in session and
            "sendReceiverFeedback" in session,
            "stream loop must send periodic receiver feedback")
    for line in ("goog-remb", "ccm fir", "max-fs=3600", "max-mbps=108000"):
        require(line in sdp and line in patch,
                f"tracked SDP must preserve {line}")
    require("(uint64_t)delta_ms" in peer and "/ 1000" in peer,
            "RTCP DLSR must use seconds, not raw milliseconds")

    print("Xbox bitrate feedback tests passed")


if __name__ == "__main__":
    main()
