#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    activity = Path("src/ui/auth_activity.cpp").read_text()
    header = Path("src/ui/auth_activity.h").read_text()
    server = Path("src/auth/xbox_login_server.cpp").read_text()
    switch_makefile = Path("Makefile.switch").read_text()

    require("XboxLoginServer" in header and "xbox_login_server_->start" in activity,
            "Xbox device-code login must own a local phone helper service")
    require("void AuthActivity::onContentAvailable()" in activity and
            "beginAuthRequest();" in activity[activity.index("void AuthActivity::onContentAvailable()"):],
            "opening the signed-out Xbox flow must request the phone login automatically")
    require("getResolvedAppLocale()" in activity,
            "the Xbox phone helper must follow the resolved app language")
    require("qr_view_->setWidth(280)" in activity and
            "qr_view_->setHeight(280)" in activity,
            "the Xbox phone-helper QR must be large enough to scan")
    require("sessionToken()" in server and '"/xbox/"' in server,
            "the Xbox helper must use an unguessable per-login URL")
    require("navigator.clipboard.writeText" in server and
            "document.execCommand('copy')" in server,
            "the phone page must provide clipboard and compatibility copy paths")
    require("poll(&listener, 1, 250)" in server,
            "the Xbox helper listener must wake periodically so stopping it cannot block the UI")
    require("const int client = accept" in server and
            server.index("poll(&listener, 1, 250)") < server.index("const int client = accept"),
            "the Xbox helper must wait with a timeout before accepting phone connections")
    require("verification_url" in server and "user_code" in server and
            "diagnosticLog" in server and "user_code.c_str()" not in server,
            "the helper must show the code without logging it")
    require("src/auth/xbox_login_server.cpp" in switch_makefile,
            "the Switch build must include the Xbox login helper")

    print("Xbox phone login tests passed")


if __name__ == "__main__":
    main()
