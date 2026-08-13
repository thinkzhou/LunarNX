#!/usr/bin/env python3
from pathlib import Path
import re


source = Path("src/app/xbox_stream_session.cpp").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


suppression = re.search(
    r"else if \(callbacks\.input_suppressed && callbacks\.input_suppressed\(\)\) \{\s*"
    r"(?P<body>.*?)\n\s*\}\n\s*const auto input_packet",
    source,
    re.DOTALL,
)
require(suppression is not None,
        "Xbox stream loop must honor the runtime input suppression callback")
body = suppression.group("body")
require("gamepad_state = {};" in body,
        "Xbox suppression must clear the complete gamepad frame")
require("gamepad_state.view = false" not in body and
        "gamepad_state.menu = false" not in body,
        "Xbox suppression must not leave buttons, triggers, or sticks active")

print("Xbox input suppression test passed")
