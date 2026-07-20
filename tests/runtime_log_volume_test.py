#!/usr/bin/env python3
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    transport = Path("src/app/web_rtc_transport.cpp").read_text()
    peer = Path("src/webrtc/peer_manager.cpp").read_text()

    noisy_messages = (
        "send input begin",
        "send input result",
        "send input datachannel sid=",
        "send input datachannel result=",
    )
    combined = transport + peer
    for message in noisy_messages:
        require(message not in combined,
                f"Per-packet runtime log must be removed: {message}")

    require("input send failed" in Path("src/app/xbox_stream_session.cpp").read_text(),
            "Bounded input failure diagnostics must remain available")

    print("runtime log volume test passed")


if __name__ == "__main__":
    main()
