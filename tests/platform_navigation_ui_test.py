#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    platform = Path("src/ui/platform_activity.cpp").read_text()
    ps = Path("src/ui/ps_activity.cpp").read_text()
    xbox = Path("src/ui/main_activity.cpp").read_text()

    require(Path("romfs/img/platform/xbox.png").is_file() and
            Path("romfs/img/platform/playstation.png").is_file() and
            Path("res/platform/xbox.svg").is_file() and
            Path("res/platform/playstation.svg").is_file(),
            "platform logo sources and runtime images must be packaged")

    require("makePlatformTile" in platform and
            "tile->setFocusable" not in platform and
            "tile->registerClickAction" in platform and
            'img/platform/xbox.png' in platform and
            'img/platform/playstation.png' in platform and
            "setImageFromRes" in platform,
            "platform selection must use whole-focus branded platform tiles")
    require("makeAppFrame" in platform and
            "workspace->setPadding(28, 48, 24, 48)" in platform and
            "makeSidebarButton" not in platform,
            "platform selection must use the centered sidebar-free Stitch layout")
    require("BUTTON_X" in platform and "BUTTON_Y" in platform,
            "settings and About must remain reachable through controller actions")
    require("new StreamSettingsActivity(nullptr, loadStreamSettings(), {}," in platform and
            "StreamSettingsScope::Global" in platform,
            "platform selection must expose global settings without constructing a controller")
    require("savedFileExists(lunar::get_token_path())" in platform and
            "savedFileExists(lunar::get_ps_credentials_path())" in platform and
            "savedFileExists(lunar::get_psn_token_path())" in platform,
            "platform tiles must show conservative saved-account readiness without networking")
    require("content_title_ = new brls::Label()" not in ps and
            "content_subtitle_ = makeMutedLabel" not in ps,
            "PS source pages must not duplicate their section headings")

    create = xbox.split("brls::View* MainActivity::createContentView()", 1)[1]
    back = create.split("workspace->registerAction", 1)[1].split(
        "workspace->registerAction", 1)[0]
    require("BUTTON_B" in back and "popActivity" in back,
            "Xbox B action must return to platform selection")
    require("resetToAuthActivity" not in back and "signOut" not in back,
            "Xbox B action must preserve the signed-in account")
    require("exit_navigation_ready_at_" in platform and
            "now < exit_navigation_ready_at_" in platform,
            "platform exit must ignore a held Back input after a child activity closes")

    require("openBrowserDiagnostic" not in platform and
            "https://www.baidu.com" not in platform and
            "new DevToolsActivity" not in platform and
            'lunarnx/about/home_entry_title' in platform and
            "new AboutActivity" in platform,
            "platform home must replace development tools with the About entry")

    print("Platform navigation UI tests passed")


if __name__ == "__main__":
    main()
