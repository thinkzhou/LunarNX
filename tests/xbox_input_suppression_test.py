#!/usr/bin/env python3
from pathlib import Path
import re


source = Path("src/app/xbox_stream_session.cpp").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


send_block_start = source.index(
    "if (control_started && connected_before_pump) {")
send_block_end = source.index(
    "if (loop_started >= next_rumble_tick)", send_block_start)
send_block = source[send_block_start:send_block_end]
route_offset = send_block.find("input_router_.route")
encode_offset = send_block.find("xinput_.encodeFrames")
require(route_offset >= 0 and encode_offset > route_offset,
        "Xbox must apply input ownership immediately before packet encoding")
require("input_suppressed" not in source,
        "Xbox must not retain a second boolean suppression path")

print("Xbox input ownership test passed")
