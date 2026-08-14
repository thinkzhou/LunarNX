#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def read(path: str) -> str:
    file_path = ROOT / path
    require(file_path.exists(), f"missing {path}")
    return file_path.read_text()


def main() -> None:
    activity_header = read("src/ui/stream_loading_activity.h")
    activity_source = read("src/ui/stream_loading_activity.cpp")
    main_header = read("src/ui/main_activity.h")
    main_source = read("src/ui/main_activity.cpp")
    switch_makefile = read("Makefile.switch")

    require("class StreamLoadingActivity : public brls::Activity" in activity_header,
            "stream startup must use a dedicated Borealis activity")
    require("ProgressSpinnerSize::LARGE" in activity_source,
            "the destination page must show Borealis' large loading spinner")
    require("registerClickAction" in activity_source,
            "the loading page must consume repeated confirm presses")
    require("ControllerButton::BUTTON_B" in activity_source and
            "requestCancel" in activity_source,
            "the loading page must expose a safe cancel action")
    require("detach()" not in activity_source,
            "the loading activity must use a stable layout root, not a detached overlay")
    require("onContentAvailable" in activity_source and
            "brls::sync" in activity_source and
            "startConnection" in activity_source,
            "connection startup must be deferred until the loading page content exists")
    require("startStream(" in activity_source and
            "startCloudStream(" in activity_source,
            "the loading activity must support home and cloud targets")
    require("new StreamView" in activity_source and
            "TransitionAnimation::NONE" in activity_source,
            "successful startup must hand off to the stream page without a Metal fade")

    require("auto* scroll = new brls::ScrollingFrame();" in main_source and
            "scroll_frame_ = scroll;" in main_source and
            "scroll->setContentView(root);" in main_source and
            "workspace->addView(scroll);" in main_source,
            "MainActivity must retain its known-good ScrollingFrame content host")
    require("StreamLoadingOverlay" not in main_header + main_source,
            "MainActivity must not construct the crashing hidden overlay")
    require("connecting_overlay_" not in main_header + main_source,
            "MainActivity must not own startup overlay views")
    require(main_source.count("if (connecting_->exchange(true))") == 2,
            "home and cloud Play actions must retain atomic duplicate-start guards")
    require(main_source.count("new StreamLoadingActivity(") == 2,
            "home and cloud Play actions must both navigate to the startup page")
    require(main_source.count("brls::TransitionAnimation::NONE") >= 2,
            "startup page navigation must avoid emulator-sensitive fades")
    require("src/ui/stream_loading_activity.cpp" in switch_makefile,
            "Switch build must include the startup activity")
    require("src/ui/stream_loading_overlay.cpp" not in switch_makefile,
            "the removed overlay must not remain in the Switch build")

    print("stream loading activity tests passed")


if __name__ == "__main__":
    main()
