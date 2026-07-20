#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("src/ui/main_activity.cpp").read_text()
    start = source.index("void MainActivity::startConsoleStream")
    end = source.index("void MainActivity::refreshConsoles", start)
    function = source[start:end]

    require("app::StreamState::Connecting" in function,
            "Connect handler must reject re-entry while connecting")
    require("app::StreamState::Streaming" in function,
            "Connect handler must reject re-entry while streaming")
    require("Connect ignored" in function,
            "Rejected duplicate Connect actions must be diagnosable")

    print("MainActivity Connect guard tests passed")


if __name__ == "__main__":
    main()
