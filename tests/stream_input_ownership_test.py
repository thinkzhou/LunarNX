#!/usr/bin/env python3
"""Regression checks for exclusive ownership of in-stream controller input."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


runtime = (ROOT / "src/app/stream_runtime.h").read_text()
view = (ROOT / "src/ui/stream_view.cpp").read_text()
xbox = (ROOT / "src/app/xbox_stream_session.cpp").read_text()
ps = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()

require("input::StreamInputRouter& inputRouter()" in runtime,
        "the shared stream runtime must expose one input-ownership router")
require("StreamInputOwner::Ui" in view and "StreamInputOwner::Game" in view,
        "the stream menu must explicitly transfer input ownership between UI and game")
require("setInputSuppressed" not in view,
        "StreamView must not maintain a second boolean suppression path")
require("input_router_.route(gamepad_state)" in xbox,
        "Xbox must route every outgoing gamepad frame through input ownership")
require("input_router_.route(state)" in ps,
        "PlayStation must route every outgoing gamepad frame through input ownership")

print("stream input ownership checks passed")
