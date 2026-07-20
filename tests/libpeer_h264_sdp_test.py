#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("lib/libpeer/src/sdp.c").read_text()
    patch = Path("tools/libpeer_legacy/legacy-libpeer-switch.patch").read_text()

    expected = (
        "a=fmtp:96 profile-level-id=42e01f;"
        "level-asymmetry-allowed=1;packetization-mode=1"
    )
    require(expected in source,
            "legacy libpeer must offer XStreaming/libwebrtc H264 packetization mode 1")
    require(expected in patch,
            "tracked legacy libpeer patch must reproduce H264 packetization mode 1")

    print("Legacy libpeer H264 SDP tests passed")


if __name__ == "__main__":
    main()
