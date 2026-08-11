#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    main_source = Path("src/main.cpp").read_text()
    platform_source = Path("src/ui/platform_activity.cpp").read_text()
    platform_header = Path("src/ui/platform_activity.h").read_text()
    auth_source = Path("src/ui/auth_activity.cpp").read_text()
    ps_header = Path("src/ui/ps_activity.h").read_text()

    normal_start = main_source.split("#else", 1)[1].split("#endif", 1)[0]
    require("new lunar::ui::PlatformActivity()" in normal_start,
            "normal startup must open platform selection")
    require("new lunar::ui::AuthActivity()" not in normal_start,
            "Xbox auth must not be the application root")

    xbox_body = platform_source.split("void PlatformActivity::openXbox()", 1)[1]
    xbox_body = xbox_body.split("void PlatformActivity::openPlayStation()", 1)[0]
    ps_body = platform_source.split("void PlatformActivity::openPlayStation()", 1)[1]
    require("new AuthActivity" in xbox_body,
            "selecting Xbox must enter the Xbox account flow")
    require("new PsActivity()" in ps_body and "StreamController" not in ps_body,
            "selecting PlayStation must not depend on an Xbox controller")
    require("PlatformActivity();" in platform_header,
            "platform selection must be constructible without Xbox state")
    require("PsActivity();" in ps_header,
            "PlayStation UI must be constructible without Xbox state")

    require("new MainActivity(ctrl)" in auth_source,
            "successful Xbox auth must enter the Xbox page")
    require("new PlatformActivity(ctrl)" not in auth_source,
            "Xbox auth must not own platform selection navigation")

    print("Platform-first startup tests passed")


if __name__ == "__main__":
    main()
