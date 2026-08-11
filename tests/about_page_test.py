#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABOUT = (ROOT / "src/ui/about_activity.cpp").read_text()
MAIN = (ROOT / "src/ui/main_activity.cpp").read_text()


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


require('wordmark->setText("LUNARNX")' in ABOUT and
        'version_chip = makeUiCard(brls::Axis::ROW)' in ABOUT,
        "About page must use the shared LunarNX brand header")
require('intro = makeUiCard(brls::Axis::ROW)' in ABOUT and
        'makeSectionHeader(' in ABOUT,
        "About content must use the shared card and section hierarchy")
require('qq_number->setText("736743823")' in ABOUT and
        'lunarnx/about/qq_group' in ABOUT,
        "About page must show the LunarNX QQ group")
require('add_feature("XBOX"' in ABOUT and
        'add_feature("XCLOUD"' in ABOUT and
        'add_feature("PS"' in ABOUT,
        "About page must describe all supported streaming platforms")
require('AGPL-3.0-only-OpenSSL' in ABOUT and
        'lunarnx/about/components' in ABOUT,
        "About page must retain license and component attribution")
require('lunarnx/common/about' in MAIN,
        "main page About button must be localized")

for locale in ("en-US", "zh-Hans", "zh-Hant"):
    text = (ROOT / f"romfs/i18n/{locale}/lunarnx.json").read_text()
    for key in ("community_title", "qq_group", "features_title",
                "open_source_title", "footer"):
        require(f'"{key}"' in text,
                f"missing About localization {key} for {locale}")

print("About page tests passed")
