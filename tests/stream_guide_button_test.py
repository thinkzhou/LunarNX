#!/usr/bin/env python3
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


reader = Path("src/input/gamepad_reader.h").read_text()
reader_impl = Path("src/input/gamepad_reader.cpp").read_text()
stream_view = Path("src/ui/stream_view.cpp").read_text()

require("applyGuideChord" in reader and "applyGuideChord(state)" in reader_impl,
        "GamepadReader must map a physical Switch chord to Xbox Guide")
require("!state.lb || !state.rb || !state.menu" in reader,
        "the Guide chord must be L + R + Plus")
require("state.guide = true" in reader,
        "the physical chord must set the Xbox Nexus/Guide state")
require("state.lb = false" in reader and
        "state.rb = false" in reader and
        "state.menu = false" in reader,
        "the Guide chord must not leak bumper/Menu presses to the Xbox")
require("class XboxGuideButton" not in stream_view and
        "TapGestureRecognizer" not in stream_view,
        "the unusable touch overlay must be removed")
require("hidsysActivateCaptureButton" not in Path("src/platform/switch_wrapper.c").read_text(),
        "the button solution must not depend on privileged Capture-button shared memory")
require("hidGetCaptureButtonStates" not in reader_impl,
        "the button solution must not depend on raw Capture-button state")

print("Stream physical Guide chord tests passed")
