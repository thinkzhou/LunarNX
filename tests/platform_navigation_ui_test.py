#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    platform = Path("src/ui/platform_activity.cpp").read_text()
    ps = Path("src/ui/ps_activity.cpp").read_text()
    xbox = Path("src/ui/main_activity.cpp").read_text()

    require("makePlatformTile" in platform and
            "tile->setFocusable" not in platform and
            "tile->registerClickAction" in platform and
            "ConsoleGlyphView" in platform,
            "platform selection must use whole-focus console tiles")
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
    back = create.split("workspace->registerAction", 1)[1].split(");", 1)[0]
    require("BUTTON_B" in back and "popActivity" in back,
            "Xbox B action must return to platform selection")
    require("resetToAuthActivity" not in back and "signOut" not in back,
            "Xbox B action must preserve the signed-in account")
    require("exit_navigation_ready_at_" in platform and
            "now < exit_navigation_ready_at_" in platform,
            "platform exit must ignore a held Back input after a child activity closes")

    require("openBrowserDiagnostic" not in platform and
            "https://www.baidu.com" not in platform and
            "new DevToolsActivity" in platform,
            "temporary browser diagnostics must be consolidated into development tools")

    print("Platform navigation UI tests passed")


if __name__ == "__main__":
    main()
