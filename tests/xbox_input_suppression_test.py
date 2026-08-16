#!/usr/bin/env python3
from pathlib import Path
import re


source = Path("src/app/xbox_stream_session.cpp").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


ownership = re.search(
    r"(?P<route>gamepad_state = input_router_\.route\(gamepad_state\);)\s*"
    r"const auto input_packet",
    source,
    re.DOTALL,
)
require(ownership is not None,
        "Xbox must apply input ownership immediately before packet encoding")
require("input_suppressed" not in source,
        "Xbox must not retain a second boolean suppression path")

print("Xbox input ownership test passed")
