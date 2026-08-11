#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    settings = (ROOT / "src/ui/stream_settings_activity.cpp").read_text()
    settings_header = (ROOT / "src/ui/stream_settings_activity.h").read_text()
    main_activity = (ROOT / "src/ui/main_activity.cpp").read_text()
    controller = (ROOT / "src/app/stream_controller.cpp").read_text()
    profile = (ROOT / "src/app/stream_profile.h").read_text()
    session_client = (ROOT / "src/app/xbox_session_client.cpp").read_text()
    api_header = (ROOT / "src/api/xbox_api_client.h").read_text()
    api_source = (ROOT / "src/api/xbox_api_client.cpp").read_text()

    require('"preferred_game_language"' in settings and
            "preferred_game_language" in settings_header,
            "Xbox settings must persist the preferred cloud game language")
    for locale in ("en-US", "ja-JP", "ko-KR", "zh-CN", "zh-TW"):
        require(f'{{"{locale}",' in settings,
                f"game language selector must expose {locale}")
    require("setPreferredGameLanguage" in main_activity,
            "saved language must be applied when the Xbox page opens")
    require("profile.locale = getPreferredGameLanguage()" in controller,
            "cloud profile must carry the selected game language")
    require("std::string locale" in profile and
            "request.locale = profile.locale" in session_client and
            "std::string locale" in api_header,
            "locale must cross the profile and API request boundaries")
    require('cJSON_AddStringToObject(settings, "locale"' in api_source and
            'cJSON_AddStringToObject(settings, "locale", "en-US")' not in api_source,
            "session creation must not hard-code the game locale")
    require('"&market=US&language=" + catalog_language_' in api_source and
            'headers["Accept-Language"] = catalog_language_' in api_source,
            "catalog metadata requests must use the selected game language")
    require('cJSON_AddStringToObject(root, "locale", locale.c_str())' in controller and
            "cached_locale != expected_locale" in controller,
            "cloud library caches must not cross language selections")
    require("game_language_changed" in main_activity and
            "refreshCurrentSource();" in main_activity,
            "changing language on the cloud page must refresh its catalog")

    print("xCloud game language test passed")


if __name__ == "__main__":
    main()
