#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = (ROOT / "src/app/stream_runtime.h").read_text()
READER_H = (ROOT / "src/ps/ps_touchpad_reader.h").read_text()
READER = (ROOT / "src/ps/ps_touchpad_reader.cpp").read_text()
CONTROLLER = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
VIEW = (ROOT / "src/ui/stream_view.cpp").read_text()


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


require("TouchpadFeedbackGesture" in RUNTIME and
        "getTouchpadFeedback() const" in RUNTIME,
        "the stream UI needs a protocol-neutral touchpad feedback snapshot")
require("screen_x" in READER_H and "screen_y" in READER_H and
        "PsTouchpadFeedback feedback() const" in READER_H,
        "the PS reader must expose the same physical touch positions it classifies")
require("GestureState::Pan" in READER and "PsTouchpadGesture::Pan" in READER and
        "GestureState::ReleaseHold" in READER and "PsTouchpadGesture::Tap" in READER and
        "PsTouchpadGesture::LongPress" in READER,
        "tap pan and long-press feedback must come from the real gesture state machine")
require("touchpad_feedback_mutex_" in CONTROLLER and
        "touchpad_feedback_ = snapshot" in CONTROLLER,
        "the 8 ms PS input thread must publish feedback through a bounded snapshot")
require("class TouchpadFeedbackView" in VIEW and
        "nvgCircle" in VIEW and "nvgMoveTo" in VIEW and "nvgLineTo" in VIEW,
        "the stream overlay must draw touch points and a short swipe trail")
require("touchpad_tap" in VIEW and "touchpad_pan" in VIEW and
        "touchpad_long_press" in VIEW and "age / 0.5f" in VIEW,
        "gesture labels must fade after roughly 500 ms")
require("kTrailPoints = 12" in VIEW and "std::move(trail.begin() + 1" in VIEW,
        "swipe trails must use fixed bounded storage")
require("StreamPlatform::PlayStation" in VIEW,
        "touch feedback must remain PlayStation-only")

print("PS touchpad feedback tests passed")
