#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    stream_view = (ROOT / "src/ui/stream_view.cpp").read_text()
    stream_overlay = (ROOT / "src/ui/stream_overlay.cpp").read_text()

    for name in ("overlay_", "perf_overlay_", "confirm_box_"):
        detach = stream_view.find(f"{name}->detach();")
        add = stream_view.find(f"root->addView({name});")
        require(detach >= 0 and add >= 0 and detach < add,
                f"{name} must be detached before it is added to the stream root")

    require("setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);" in stream_overlay,
            "The detached top status bar must have an explicit full-screen width")

    print("stream overlay layout test passed")


if __name__ == "__main__":
    main()
