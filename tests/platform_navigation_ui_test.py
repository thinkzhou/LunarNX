#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    platform = Path("src/ui/platform_activity.cpp").read_text()
    ps = Path("src/ui/ps_activity.cpp").read_text()
    xbox = Path("src/ui/main_activity.cpp").read_text()

    require("makePlatformRow" in platform and
            "button->setWidth(180)" in platform and
            "copy->setGrow(1.0f)" in platform,
            "platform cards must reserve separate text and button columns")
    require("content_title_ = new brls::Label()" not in ps and
            "content_subtitle_ = makeMutedLabel" not in ps,
            "PS source pages must not duplicate their section headings")

    create = xbox.split("brls::View* MainActivity::createContentView()", 1)[1]
    back = create.split("scroll->registerAction", 1)[1].split(");", 1)[0]
    require("BUTTON_B" in back and "popActivity" in back,
            "Xbox B action must return to platform selection")
    require("resetToAuthActivity" not in back and "signOut" not in back,
            "Xbox B action must preserve the signed-in account")
    require("exit_navigation_ready_at_" in platform and
            "now < exit_navigation_ready_at_" in platform,
            "platform exit must ignore a held Back input after a child activity closes")

    print("Platform navigation UI tests passed")


if __name__ == "__main__":
    main()
