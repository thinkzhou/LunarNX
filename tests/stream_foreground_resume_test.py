#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


view = (ROOT / "src/ui/stream_view.cpp").read_text()
view_header = (ROOT / "src/ui/stream_view.h").read_text()
runtime = (ROOT / "src/app/stream_runtime.h").read_text()
xbox = (ROOT / "src/app/stream_controller.cpp").read_text()
ps = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()

require("disableScreenDimming(true)" in view,
        "stream view must prevent automatic screen dimming")
require("disableScreenDimming(false)" in view,
        "stream view must restore automatic screen dimming")
require("getWindowFocusChangedEvent()->subscribe" in view,
        "stream view must subscribe to Switch foreground changes")
require("getWindowFocusChangedEvent()->unsubscribe" in view,
        "stream view must unsubscribe before destruction")
require("backgrounded_" in view_header,
        "stream view must retain background state while the app is suspended")
require("resumeAfterForeground" in runtime,
        "protocol-neutral runtime must expose foreground recovery")
require("StreamController::resumeAfterForeground" in xbox,
        "Xbox runtime must implement foreground recovery")
require("PsStreamController::resumeAfterForeground" in ps,
        "PlayStation runtime must implement foreground recovery")
require("!backgrounded_.load()" in view,
        "disconnect handling must be deferred while the app is backgrounded")
recovery_claim = view.index("foreground_recovery_running_.exchange(true)")
background_clear = view.index("backgrounded_ = false")
require(recovery_claim < background_clear,
        "foreground recovery must be claimed before disconnect handling resumes")

print("stream foreground resume regression checks passed")
