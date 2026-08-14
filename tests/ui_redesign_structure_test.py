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
    create_window = main_entry.find('brls::Application::createWindow("LunarNX")')
    set_dark_theme = main_entry.find("setThemeVariant(brls::ThemeVariant::DARK)")
    require(create_window >= 0, "main must create the Borealis window")
    require(set_dark_theme > create_window,
            "dark theme must be selected after createWindow to avoid Switch startup crashes")
    style_header = read("src/ui/ui_style.h")
    style_source = read("src/ui/ui_style.cpp")
    auth_source = read("src/ui/auth_activity.cpp")
    platform_source = read("src/ui/platform_activity.cpp")
    main_source = read("src/ui/main_activity.cpp")
    main_header = read("src/ui/main_activity.h")
    settings_header = read("src/ui/stream_settings_activity.h")
    settings_source = read("src/ui/stream_settings_activity.cpp")
    loading_source = read("src/ui/stream_loading_activity.cpp")
    ps_source = read("src/ui/ps_activity.cpp")
    ps_settings_source = read("src/ui/ps_settings_activity.cpp")
    psn_login_source = read("src/ui/psn_login_activity.cpp")
    registration_source = read("src/ui/ps_registration_activity.cpp")
    about_source = read("src/ui/about_activity.cpp")
    mapping_source = read("src/ui/button_mapping_activity.cpp")
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
    for color in (
        "nvgRGB(27, 30, 35)", "nvgRGB(35, 39, 46)",
        "nvgRGB(38, 43, 50)", "nvgRGB(0, 217, 195)",
        "nvgRGB(72, 246, 223)", "nvgRGB(154, 163, 173)",
        "nvgRGB(58, 65, 75)",
    ):
        require(color in style_source, f"approved UI token missing: {color}")
    require("makeFlatSection" in style_header and "addFlatRow" in style_header,
            "flat sections and divider rows must be reusable")
    require("class SidebarButton" in style_source and
            "nvgRect(vg, x, y + 8, 4, height - 16)" in style_source,
            "active sidebar items need the approved four-pixel accent bar")
    require("enum class UiIcon" in style_header and
            "UiIcon::Console" in main_source and "UiIcon::Cloud" in ps_source,
            "sidebar navigation must use local vector icons")
    require("ConsoleGlyphView" in style_header,
            "Remote Play cards need a code-native console identity view")

    require('#include "ui_style.h"' in auth_source,
            "authentication page must use the shared visual system")
    require('"lunarnx/auth/tagline"' in auth_source and
            '"lunarnx/auth/sign_in"' in auth_source,
            "authentication page must use the approved branded hierarchy")
    for name, source in (
        ("platform", platform_source), ("Xbox", main_source),
        ("PlayStation", ps_source), ("global settings", settings_source),
        ("PS settings", ps_settings_source), ("Xbox auth", auth_source),
        ("PSN login", psn_login_source), ("PS registration", registration_source),
        ("About", about_source), ("button mapping", mapping_source),
    ):
        require("makeAppFrame" in source,
                f"{name} Activity must use the shared AppletFrame shell")

    require("class StreamSettingsActivity : public brls::Activity" in settings_header,
            "stream settings must live in a dedicated Activity")
    require("SelectorCell" in settings_source and "BooleanCell" in settings_source,
            "settings page must use controller-friendly Borealis cells")
    require("SelectorCell" in settings_source and "BooleanCell" in settings_source,
            "settings must retain controller-first selectable rows")
    require("SelectorCell" in ps_settings_source,
            "PlayStation settings must retain controller-first selectable rows")
    require("makeUiCard" not in mapping_source and
            "makeFlatSection" in mapping_source and "addFlatRow" in mapping_source,
            "button mapping must use the shared flat list hierarchy")
    require("ButtonMappingProfile::Xbox" in settings_source and
            "ButtonMappingProfile::PlayStation" in ps_settings_source,
            "settings must keep Xbox and PlayStation mapping profiles separate")
    require("StreamSettingsScope::Global" in platform_source and
            '"lunarnx/common/xbox_settings"' in main_source and
            '"lunarnx/common/playstation_settings"' in ps_source,
            "navigation must make global Xbox and PlayStation settings scope explicit")
    require("StreamSettingsScope::Global" in platform_source and
            "scope_ == StreamSettingsScope::Global" in settings_source and
            '"lunarnx/common/xbox_settings"' in settings_source,
            "global settings must be semantically separate from Xbox stream settings")
    require("Console Type" in registration_source and
            "PlayStation 5" in registration_source and
            "PlayStation 4" in registration_source,
            "pairing flow must retain explicit console type selection context")
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
    require("loading_card_" in loading_source and
            "loading_card_->setBorderThickness(1)" in loading_source and
            "loading_card_->setCornerRadius(8)" in loading_source,
            "loading state must use the centered outlined status card from Stitch")
    require("class PsConnectActivity" in ps_source and
            "root->setBackgroundColor" in ps_source,
            "PlayStation connection state must share the themed transient surface")
    require('#include "ui_style.h"' in stream_overlay + perf_overlay,
            "stream overlays must share the LunarNX palette")
    require("root->registerAction" in stream_view and
            "this->registerAction" not in stream_view,
            "stream actions must be registered on the available root view")
    require("setDetachedPosition" in stream_view and
            "ORIGINAL_WINDOW_WIDTH - 520" in stream_view,
            "stream quick menu must remain centered")

    require("src/ui/ui_style.cpp" in switch_makefile and
            "src/ui/stream_settings_activity.cpp" in switch_makefile,
            "Switch build must include the design system and settings Activity")

    print("UI redesign structure tests passed")


if __name__ == "__main__":
    main()
