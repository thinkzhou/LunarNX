#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABOUT = (ROOT / "src/ui/about_activity.cpp").read_text()
MAIN = (ROOT / "src/ui/main_activity.cpp").read_text()


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


require('product->setText("LunarNX")' in ABOUT and
        'version->setText("v" LUNARNX_VERSION)' in ABOUT and
        'makeAppFrame(brls::getStr("lunarnx/common/about"), root)' in ABOUT,
        "About page must have one frame title and compact product identity")
require('ScrollingFrame' not in ABOUT and
        'capabilities = new brls::Box(brls::Axis::ROW)' in ABOUT and
        ABOUT.count('makeInfoCell(') >= 4,
        "About content must fit a single screen with equal capability cells")
require('"736743823"' in ABOUT and
        'lunarnx/about/qq_group' in ABOUT,
        "About page must show the LunarNX QQ group")
require('"XBOX"' in ABOUT and '"XCLOUD"' in ABOUT and
        '"PLAYSTATION"' in ABOUT,
        "About page must describe all supported streaming platforms")
require('AGPL-3.0-only-OpenSSL' in ABOUT and
        'github.com/thinkzhou/LunarNX' in ABOUT and
        'lunarnx/about/acknowledgements' in ABOUT,
        "About page must retain license and component attribution")
require('lunarnx/common/about' in MAIN,
        "main page About button must be localized")

for locale in ("en-US", "zh-Hans", "zh-Hant"):
    text = (ROOT / f"romfs/i18n/{locale}/lunarnx.json").read_text()
    for key in ("qq_group", "features_title", "project_title", "repository",
                "license", "acknowledgements", "components_short",
                "build_label"):
        require(f'"{key}"' in text,
                f"missing About localization {key} for {locale}")

print("About page tests passed")
