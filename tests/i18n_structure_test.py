#!/usr/bin/env python3
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LOCALES = ("en-US", "zh-Hans", "zh-Hant")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def flatten(value, prefix=""):
    result = {}
    for key, child in value.items():
        path = f"{prefix}/{key}" if prefix else key
        if isinstance(child, dict):
            result.update(flatten(child, path))
        else:
            require(isinstance(child, str), f"translation {path} must be a string")
            result[path] = child
    return result


def placeholders(value: str):
    return re.findall(r"\{(?:\d+)?(?::[^}]*)?\}", value)


def main() -> None:
    catalogs = {}
    for locale in LOCALES:
        path = ROOT / "romfs/i18n" / locale / "lunarnx.json"
        require(path.exists(), f"missing LunarNX catalog for {locale}")
        catalogs[locale] = flatten(json.loads(path.read_text()))

    english_keys = set(catalogs["en-US"])
    require(len(english_keys) >= 80, "catalog should cover the complete user-facing UI")
    for locale in LOCALES[1:]:
        require(set(catalogs[locale]) == english_keys,
                f"{locale} keys must exactly match en-US")
        for key in english_keys:
            require(placeholders(catalogs[locale][key]) ==
                    placeholders(catalogs["en-US"][key]),
                    f"format placeholders differ for {locale}:{key}")

    traditional_hints_path = ROOT / "romfs/i18n/zh-Hant/hints.json"
    require(traditional_hints_path.exists(),
            "Traditional Chinese must include Borealis control hints")
    traditional_hints = json.loads(traditional_hints_path.read_text())
    for key in ("ok", "cancel", "back", "on", "off"):
        require(isinstance(traditional_hints.get(key), str),
                f"Traditional Chinese Borealis hint is missing: {key}")

    main_source = (ROOT / "src/main.cpp").read_text()
    locale_source = (ROOT / "src/ui/i18n.cpp").read_text()
    settings_source = (ROOT / "src/ui/stream_settings_activity.cpp").read_text()
    makefile = (ROOT / "Makefile.switch").read_text()
    default_config = (ROOT / "config/default_config.json").read_text()

    require("configureAppLocale();" in main_source,
            "startup must configure the locale")
    require(main_source.index("configureAppLocale();") <
            main_source.index("brls::Application::init()"),
            "locale override must be applied before Borealis loads translations")
    for locale in ("auto", "en-US", "zh-Hans", "zh-Hant"):
        require(f'"{locale}"' in locale_source,
                f"locale helper must support {locale}")
    require("APP_LOCALE_DEFAULT" in locale_source,
            "locale helper must configure Borealis")
    require('"romfs:/i18n/"' in locale_source and
            "setGetSystemLanguage" in locale_source,
            "restart notice must load the selected or system locale catalog")
    require("saveConfiguredLanguage" in settings_source and
            "language_restart" in locale_source,
            "settings must persist the language and show a restart notice")
    require("getLanguageRestartNotice(kLanguages[selected].value)" in
            settings_source,
            "restart notice must use the newly selected language")
    require("Application::quit" not in settings_source,
            "changing language must not force the application to exit")
    require("src/ui/i18n.cpp" in makefile,
            "Switch build must compile the locale helper")
    require('"language": "auto"' in default_config,
            "default config must document system-language behavior")

    for source_name in (
        "auth_activity.cpp",
        "main_activity.cpp",
        "stream_loading_activity.cpp",
        "stream_settings_activity.cpp",
        "stream_view.cpp",
        "stream_overlay.cpp",
        "perf_overlay.cpp",
    ):
        source = (ROOT / "src/ui" / source_name).read_text()
        require("brls::getStr(\"lunarnx/" in source,
                f"{source_name} must use the LunarNX translation catalog")

    referenced_keys = set()
    for source_path in (ROOT / "src/ui").glob("*.cpp"):
        referenced_keys.update(re.findall(
            r'brls::getStr\("lunarnx/([^"\n]+)"',
            source_path.read_text()))
    missing_keys = sorted(referenced_keys - english_keys)
    require(not missing_keys,
            f"UI references translation keys missing from the catalog: {missing_keys}")

    print("LunarNX i18n structure tests passed")


if __name__ == "__main__":
    main()
