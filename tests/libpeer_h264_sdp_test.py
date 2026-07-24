#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("lib/libpeer/src/sdp.c").read_text()
    patch = Path("tools/libpeer_legacy/legacy-libpeer-switch.patch").read_text()

    expected_parts = (
        "a=fmtp:102 level-asymmetry-allowed=0;packetization-mode=1;",
        "profile-level-id=42e01f;max-fs=3600;max-mbps=108000",
    )
    require(all(part in source for part in expected_parts),
            "legacy libpeer must offer the complete XStreaming H264 capability template")
    require(all(part in patch for part in expected_parts),
            "tracked legacy libpeer patch must reproduce the H264 capability template")
    for line in (
        "a=rtcp-fb:102 goog-remb",
        "a=rtcp-fb:102 ccm fir",
        "a=rtcp-fb:102 nack pli",
        "a=rtcp-rsize",
    ):
        require(line in source and line in patch,
                f"legacy libpeer SDP must include {line}")

    print("Legacy libpeer H264 SDP tests passed")


if __name__ == "__main__":
    main()
