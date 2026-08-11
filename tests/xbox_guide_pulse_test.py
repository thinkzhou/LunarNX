#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    session = Path("src/app/xbox_stream_session.cpp").read_text()
    require("guide_pulse_frames_remaining" in session,
            "menu Guide requests must be stretched across input frames")
    require("kGuidePulseFrames" in session and "guide_pulse_frames_remaining--" in session,
            "Guide pulse must last for a bounded number of frames")
    require("gamepad_state = {};" in session and "gamepad_state.guide = true;" in session,
            "Guide pulse must suppress menu input and send Nexus")
    require("guide_release_pending" in session,
            "Guide pulse must include an explicit release frame")

    print("Xbox Guide pulse tests passed")


if __name__ == "__main__":
    main()
