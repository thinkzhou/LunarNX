#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    settings_header = (ROOT / "src/ui/stream_settings_activity.h").read_text()
    settings_source = (ROOT / "src/ui/stream_settings_activity.cpp").read_text()
    controller_header = (ROOT / "src/app/stream_controller.h").read_text()
    auth_source = (ROOT / "src/ui/auth_activity.cpp").read_text()
    config = (ROOT / "config/default_config.json").read_text()

    require("vibration_enabled" in settings_header and
            "rumble_strength_percent" in settings_header,
            "stream settings snapshot must carry rumble preferences")
    require("BooleanCell" in settings_source and
            '"lunarnx/settings/vibration"' in settings_source,
            "settings UI must expose a vibration switch")
    for strength in (25, 50, 75, 100):
        require(str(strength) in settings_source,
                f"settings UI must expose {strength}% vibration strength")
    require("saveStreamSettings(settings_)" in settings_source and
            '"vibration"' in settings_source and
            '"rumble_strength_percent"' in settings_source,
            "rumble settings must persist with the stream settings snapshot")
    require("setRumbleEnabled" in controller_header and
            "setRumbleStrengthPercent" in controller_header,
            "stream controller must own runtime rumble preferences")
    require('cJSON_GetObjectItem(root, "vibration")' in auth_source and
            'cJSON_GetObjectItem(root, "rumble_strength_percent")' in auth_source,
            "startup must restore persisted rumble preferences")
    require('"rumble_strength_percent": 50' in config,
            "default rumble strength must preserve LunarNX's existing 50% scale")

    print("rumble settings tests passed")


if __name__ == "__main__":
    main()
