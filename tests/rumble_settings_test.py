#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    settings_header = (ROOT / "src/ui/stream_settings_activity.h").read_text()
    settings_source = (ROOT / "src/ui/stream_settings_activity.cpp").read_text()
    ps_settings_header = (ROOT / "src/ui/ps_settings_activity.h").read_text()
    ps_settings_source = (ROOT / "src/ui/ps_settings_activity.cpp").read_text()
    controller_header = (ROOT / "src/app/stream_controller.h").read_text()
    ps_controller_header = (ROOT / "src/ps/ps_stream_controller.h").read_text()
    ps_controller_source = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
    ps_session_source = (ROOT / "src/ps/ps_stream_session.cpp").read_text()
    ps_bridge_source = (ROOT / "src/ps/ps_media_bridge.cpp").read_text()
    ps_activity = (ROOT / "src/ui/ps_activity.cpp").read_text()
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
    require("setRumbleEnabled" in ps_controller_header and
            "setRumbleStrengthPercent" in ps_controller_header and
            "setRumbleForwarder" in ps_controller_source and
            "setHapticsForwarder" in ps_controller_source and
            "!input_router_.gameHasInput()" in ps_controller_source and
            "state_.load() != app::StreamState::Streaming" in ps_controller_source and
            "rumble_->update()" in ps_controller_source and
            "rumble_->stop()" in ps_controller_source,
            "PS streams must forward Chiaki rumble through Switch HD rumble")
    require("bridge_.setEventForwarder" in ps_session_source and
            "bridge_.eventCallback()" in ps_session_source and
            "bridge_.hapticsSink()" in ps_session_source and
            "chiaki_session_set_haptics_sink" in ps_session_source and
            "&bridge_" in ps_session_source and
            "event->type == CHIAKI_EVENT_RUMBLE" in ps_bridge_source,
            "the registered Chiaki callbacks must pass rumble and PS5 haptics through the bridge")
    require("kPsLegacyRumbleTimeoutMs" in ps_controller_source and
            "kPsHapticsRumbleTimeoutMs" in ps_controller_source and
            "0.0f, 0.0f, 30, 0, 0" not in ps_controller_source,
            "PS rumble must latch long enough for state events and haptics packet jitter")
    require("rumble_input_suppressed_" in ps_controller_header and
            "input_suppressed != rumble_input_suppressed_" in ps_controller_source,
            "opening stream UI must stop a latched PlayStation rumble command once")
    require("connect_info_.enable_keyboard = false" in ps_session_source,
            "PS sessions must not advertise the unimplemented remote keyboard")
    require("vibration_enabled" in ps_settings_header and
            "rumble_strength_percent" in ps_settings_header and
            '"ps_vibration_enabled"' in ps_settings_source and
            '"ps_rumble_strength_percent"' in ps_settings_source,
            "PlayStation rumble preferences must persist separately from Xbox")
    require("loadPsSettings()" in ps_activity and
            "controller->setRumbleEnabled" in ps_activity and
            "controller->setRumbleStrengthPercent" in ps_activity,
            "PS streams must apply the persisted PlayStation rumble settings")
    require('cJSON_GetObjectItem(root, "vibration")' in auth_source and
            'cJSON_GetObjectItem(root, "rumble_strength_percent")' in auth_source,
            "startup must restore persisted rumble preferences")
    require('"rumble_strength_percent": 50' in config,
            "default rumble strength must preserve LunarNX's existing 50% scale")

    print("rumble settings tests passed")


if __name__ == "__main__":
    main()
