#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    source = (ROOT / "src/ui/stream_view.cpp").read_text()

    root_start = source.find("auto* root = new brls::Box")
    require(root_start >= 0, "StreamView must create a Borealis root view")
    root_setup = source[root_start:root_start + 500]

    require("root->setFocusable(true);" in root_setup,
            "StreamView root must take focus from the underlying activity")
    require("root->setHideHighlight(true);" in root_setup,
            "StreamView root focus must not draw over the video")

    print("stream view focus test passed")


if __name__ == "__main__":
    main()
