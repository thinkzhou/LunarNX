#!/usr/bin/env python3

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    entry = (ROOT / "src/main.cpp").read_text()
    activity = (ROOT / "src/ui/applet_mode_activity.cpp").read_text()
    makefile = (ROOT / "Makefile.switch").read_text()

    require("hasFullApplicationResources" in entry,
            "startup must classify the current applet type")
    require("AppletType_Application" in entry and
            "AppletType_SystemApplication" in entry,
            "only full application modes may enter the normal UI")
    require("new lunar::ui::AppletModeActivity()" in entry,
            "limited launch modes must open the instruction page")
    require(entry.index("hasFullApplicationResources(applet_type)") <
            entry.index("new lunar::ui::PlatformActivity()"),
            "the launch-mode gate must run before the normal platform page")
    require("brls::Application::quit();" in activity,
            "the instruction page must let the user exit")
    require("BUTTON_B" in activity,
            "the instruction page must support the standard B exit action")
    require("src/ui/applet_mode_activity.cpp" in makefile,
            "the Switch build must include the instruction page")

    required_keys = {
        "eyebrow", "title", "description", "step1_title", "step1_detail",
        "step2_title", "step2_detail", "step3_title", "step3_detail",
        "note", "exit",
    }
    for locale in ("en-US", "zh-Hans", "zh-Hant"):
        translations = json.loads(
            (ROOT / "romfs/i18n" / locale / "lunarnx.json").read_text()
        )
        require(required_keys <= translations.get("launch_mode", {}).keys(),
                f"{locale} must translate the launch-mode instruction page")

    print("Applet launch gate test passed")


if __name__ == "__main__":
    main()
