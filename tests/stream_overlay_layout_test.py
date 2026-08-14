#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    stream_view = (ROOT / "src/ui/stream_view.cpp").read_text()
    stream_overlay = (ROOT / "src/ui/stream_overlay.cpp").read_text()

    for name in ("overlay_", "perf_overlay_", "quick_menu_"):
        detach = stream_view.find(f"{name}->detach();")
        add = stream_view.find(f"root->addView({name});")
        require(detach >= 0 and add >= 0 and detach < add,
                f"{name} must be detached before it is added to the stream root")

    require("(brls::Application::ORIGINAL_WINDOW_WIDTH - 520) / 2" in stream_view and
            "menu_disconnect_confirm" in stream_view,
            "the centered stream menu must retain disconnect confirmation")

    require("setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);" in stream_overlay,
            "The detached top status bar must have an explicit full-screen width")
    require("setJustifyContent(brls::JustifyContent::CENTER)" in stream_overlay and
            "setHorizontalAlign(brls::HorizontalAlign::CENTER)" in stream_overlay,
            "performance metrics must be symmetrically centered at the top")
    require('"lunarnx/stream/live"' not in stream_overlay and
            '"lunarnx/stream/details_stop"' not in stream_overlay,
            "the compact performance HUD must not show live status or control hints")
    require("makeUiCard" not in stream_overlay and
            "setBackgroundColor" not in stream_overlay,
            "the compact performance HUD must render text without a background")
    require("setFontSize(12.0f)" in stream_overlay,
            "the background-free performance text must remain readable")

    print("stream overlay layout test passed")


if __name__ == "__main__":
    main()
