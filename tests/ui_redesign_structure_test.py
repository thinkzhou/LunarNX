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
    main_entry = read("src/main.cpp")
    style_header = read("src/ui/ui_style.h")
    style_source = read("src/ui/ui_style.cpp")
    auth_source = read("src/ui/auth_activity.cpp")
    main_source = read("src/ui/main_activity.cpp")
    main_header = read("src/ui/main_activity.h")
    settings_header = read("src/ui/stream_settings_activity.h")
    settings_source = read("src/ui/stream_settings_activity.cpp")
    loading_source = read("src/ui/stream_loading_activity.cpp")
    stream_overlay = read("src/ui/stream_overlay.cpp")
    perf_overlay = read("src/ui/perf_overlay.cpp")
    stream_view = read("src/ui/stream_view.cpp")
    switch_makefile = read("Makefile.switch")

    require("installLunarTheme" in main_entry,
            "application startup must install the shared LunarNX theme")
    require("struct UiPalette" in style_header and
            "Theme::getDarkTheme" in style_source and
            "Theme::getLightTheme" in style_source,
            "shared UI style must support Borealis light and dark themes")
    require("makeUiCard" in style_header and "makeSectionHeader" in style_header,
            "shared cards and section hierarchy must be reusable")
    require("ConsoleGlyphView" in style_header,
            "Remote Play cards need a code-native console identity view")

    require('#include "ui_style.h"' in auth_source,
            "authentication page must use the shared visual system")
    require('"lunarnx/auth/tagline"' in auth_source and
            '"lunarnx/auth/sign_in"' in auth_source,
            "authentication page must use the approved branded hierarchy")

    require("class StreamSettingsActivity : public brls::Activity" in settings_header,
            "stream settings must live in a dedicated Activity")
    require("SelectorCell" in settings_source and "BooleanCell" in settings_source,
            "settings page must use controller-friendly Borealis cells")
    require("new StreamSettingsActivity" in main_source,
            "MainActivity must navigate to the dedicated settings page")
    require("res_720_" not in main_header + main_source and
            "video_backend_btn_" not in main_header + main_source and
            "region_btn_" not in main_header + main_source,
            "home content must no longer contain inline stream settings controls")
    require("ConsoleGlyphView" in main_source and "makeUiCard" in main_source,
            "home console content must use the new card hierarchy")

    require('#include "ui_style.h"' in loading_source,
            "stream startup page must share the LunarNX palette")
    require('#include "ui_style.h"' in stream_overlay + perf_overlay,
            "stream overlays must share the LunarNX palette")
    require("root->registerAction" in stream_view and
            "this->registerAction" not in stream_view,
            "stream actions must be registered on the available root view")

    require("src/ui/ui_style.cpp" in switch_makefile and
            "src/ui/stream_settings_activity.cpp" in switch_makefile,
            "Switch build must include the design system and settings Activity")

    print("UI redesign structure tests passed")


if __name__ == "__main__":
    main()
