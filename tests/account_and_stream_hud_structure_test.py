#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    main_source = (ROOT / "src/ui/main_activity.cpp").read_text()
    overlay_header = (ROOT / "src/ui/stream_overlay.h").read_text()
    overlay_source = (ROOT / "src/ui/stream_overlay.cpp").read_text()

    require("account_chip" in main_source and
            '"lunarnx/common/account"' in main_source,
            "the signed-in identity must have a dedicated top-right account chip")
    require(main_source.count("gamer_tag_->setText(") == 3,
            "content loading and result counts must not overwrite account identity")

    require(overlay_source.count("makeUiCard") == 1,
            "the always-visible stream HUD must be one continuous card")
    require("health_label_" not in overlay_header and
            "Axis::COLUMN" not in overlay_source,
            "the always-visible stream HUD must remain a compact single row")

    print("Account and stream HUD structure tests passed")


if __name__ == "__main__":
    main()
