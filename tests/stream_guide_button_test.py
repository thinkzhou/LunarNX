#!/usr/bin/env python3
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


mapping = Path("src/input/button_mapping.cpp").read_text()
reader_impl = Path("src/input/gamepad_reader.cpp").read_text()
stream_view = Path("src/ui/stream_view.cpp").read_text()
controller = Path("src/app/stream_controller.h").read_text()
controller_impl = Path("src/app/stream_controller.cpp").read_text()
session_header = Path("src/app/xbox_stream_session.h").read_text()
session = Path("src/app/xbox_stream_session.cpp").read_text()

require("RemoteButton::Guide" in mapping and
        "HidNpadButton_L | HidNpadButton_R | HidNpadButton_Plus" in mapping,
        "the default Guide mapping must remain L + R + Plus")
require("consumed |= mapping" in reader_impl and
        "return combo || (consumed & mapping) == 0" in reader_impl,
        "a mapped combination must consume its single-button components")
require("btns &= ~(HidNpadButton_Minus | HidNpadButton_Plus)" in reader_impl and
        reader_impl.index("btns &= ~(HidNpadButton_Minus | HidNpadButton_Plus)") <
        reader_impl.index("auto mapped"),
        "the physical quick-menu chord must be reserved before mapping")
require("menu_xbox_button" in stream_view and
        "runtime_->requestPlatformHomeButton()" in stream_view,
        "the stream menu must expose an Xbox Guide button")
require("requestGuideButton" in controller and
        "requestPlatformHomeButton" in controller and
        "requestGuideButton();" in controller_impl and
        "guide_button_requested_.exchange(false)" in controller_impl,
        "the runtime home action must forward to the one-shot Guide request")
require("consume_guide_button" in session_header and
        "gamepad_state.guide = true" in session,
        "the stream input loop must consume the menu Guide request")
require("class XboxGuideButton" not in stream_view and
        "TapGestureRecognizer" not in stream_view,
        "the unusable touch overlay must be removed")
require("hidsysActivateCaptureButton" not in Path("src/platform/switch_wrapper.c").read_text(),
        "the button solution must not depend on privileged Capture-button shared memory")
require("hidGetCaptureButtonStates" not in reader_impl,
        "the button solution must not depend on raw Capture-button state")

print("Stream physical Guide chord tests passed")
