#!/usr/bin/env python3
"""Architecture contract for the protocol-neutral in-stream UI boundary."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


runtime = (ROOT / "src/app/stream_runtime.h").read_text()
controller = (ROOT / "src/app/stream_controller.h").read_text()
controller_impl = (ROOT / "src/app/stream_controller.cpp").read_text()
view_header = (ROOT / "src/ui/stream_view.h").read_text()
view_impl = (ROOT / "src/ui/stream_view.cpp").read_text()

require("class IStreamRuntime" in runtime,
        "the shared stream UI needs an explicit runtime contract")
require(all(name not in runtime.lower() for name in
            ("webrtc", "libpeer", "chiaki")),
        "the runtime contract must not expose protocol implementation details")
require("stream_controller.h" not in view_header and
        "app::StreamController" not in view_header and
        "app::StreamController" not in view_impl,
        "StreamView must not depend on the concrete Xbox controller")
require("../app/stream_runtime.h" in view_header and
        "std::shared_ptr<app::IStreamRuntime> runtime_" in view_header,
        "StreamView must own the protocol-neutral runtime")
require("class StreamController : public IStreamRuntime" in controller,
        "the stable Xbox controller must implement the shared runtime")
require("void StreamController::requestPlatformHomeButton()" in controller_impl and
        "requestGuideButton();" in controller_impl,
        "the neutral home action must forward to the existing Xbox Guide path")

print("Stream runtime boundary tests passed")
