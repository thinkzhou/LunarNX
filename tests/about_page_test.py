#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABOUT = (ROOT / "src/ui/about_activity.cpp").read_text()
QQ_GROUP_IMAGE = ROOT / "romfs/img/community/qq.png"
WECHAT_PAY_IMAGE = ROOT / "romfs/img/support/wechat.png"
ALIPAY_IMAGE = ROOT / "romfs/img/support/alipay.png"
MAIN = (ROOT / "src/ui/main_activity.cpp").read_text()
PLATFORM = (ROOT / "src/ui/platform_activity.cpp").read_text()


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


require('makeLabel("LunarNX"' in ABOUT and
        'makeLabel("v" LUNARNX_VERSION' in ABOUT and
        'makeAppFrame(brls::getStr("lunarnx/common/about"), workspace)' in ABOUT,
        "About page must retain a compact LunarNX identity and shared frame")
require("Tab::Project" in ABOUT and "Tab::Changelog" in ABOUT and
        "Tab::Community" in ABOUT and "Tab::Support" in ABOUT and
        "BUTTON_LB" in ABOUT and
        "BUTTON_RB" in ABOUT,
        "About page must expose controller-first project, changelog, community, and support tabs")
require("button->setGrow(1.0f)" in ABOUT and
        "tab_community_" in ABOUT,
        "About tabs must expand evenly across the available width")
require('"thinkzhou"' in ABOUT and "LunarNX Team" not in ABOUT and
        'https://github.com/thinkzhou/LunarNX' in ABOUT and
        "QrCodeView" in ABOUT and "makeQrCode(kRepositoryUrl)" in ABOUT,
        "About project tab must identify thinkzhou and prominently expose the repository")
require('"chiaki-ng"' in ABOUT and '"Borealis"' in ABOUT and
        '"libpeer"' in ABOUT and '"FFmpeg"' in ABOUT and
        "AGPL-3.0-only-OpenSSL" in ABOUT,
        "About project tab must retain dependency URL and license attribution")
require("makeDependencyTile" not in ABOUT and
        '"XStreaming"' in ABOUT and '"github.com/Geocld/XStreaming"' in ABOUT and
        '"PeaSyo"' in ABOUT and '"github.com/Geocld/PeaSyo"' in ABOUT and
        '"Moonlight-Switch"' in ABOUT and
        '"libnx"' in ABOUT and '"deko3d"' in ABOUT and
        '"wiliwili"' in ABOUT,
        "About project acknowledgements must use one expanded single-column list")
require("makeReleaseCard" in ABOUT and "release_030_notes" in ABOUT and
        "release_020_notes" in ABOUT and
        "release_010_notes" in ABOUT and "kReleasesUrl" in ABOUT,
        "About changelog tab must expose versioned notes and the release URL")
require('"img/support/wechat.png"' in ABOUT and
        '"img/support/alipay.png"' in ABOUT and
        "resourceExists(resource)" in ABOUT and
        "payment_qr_missing" in ABOUT,
        "About support tab must provide safe WeChat and Alipay QR resource slots")
require(all(image.is_file() and
            image.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
            for image in (WECHAT_PAY_IMAGE, ALIPAY_IMAGE)),
        "WeChat and Alipay payment images must be bundled as PNG resources")
require('"img/community/qq.png"' in ABOUT and
        '"QQ 736743823"' in ABOUT and
        '"https://discord.gg/cFZj8mpg2K"' in ABOUT and
        "makeQrCode(qr_payload)" in ABOUT and
        "community_qr_missing" in ABOUT,
        "About community tab must provide a QQ slot and runtime Discord QR")
require(QQ_GROUP_IMAGE.is_file() and
        QQ_GROUP_IMAGE.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"),
        "QQ group image must be bundled as a PNG resource")
require("new DevToolsActivity" not in PLATFORM and
        "lunarnx/about/home_entry_title" in PLATFORM and
        "new AboutActivity" in PLATFORM,
        "platform home must replace the development-tools tile with About")
require("lunarnx/common/about" in MAIN,
        "Xbox page About button must remain localized")

for locale in ("en-US", "zh-Hans", "zh-Hant"):
    text = (ROOT / f"romfs/i18n/{locale}/lunarnx.json").read_text()
    for key in (
        "home_entry_title", "tab_project", "tab_changelog", "tab_community",
        "tab_support",
        "author_role", "project_description", "source_code", "releases",
        "open_source_projects", "release_030_notes", "release_020_notes",
        "release_010_notes",
        "international_group", "international_hint", "community_qr_missing",
        "community_privacy", "support_message", "wechat_pay", "alipay",
        "payment_qr_missing",
    ):
        require(f'"{key}"' in text,
                f"missing About localization {key} for {locale}")
    require("LunarNX Team" not in text and "thinkzhou" in text,
            f"About developer credit must be thinkzhou for {locale}")

print("About page tests passed")
