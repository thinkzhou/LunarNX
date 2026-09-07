#!/usr/bin/env python3
"""Static UI wiring contracts, not a Borealis or Steam end-to-end test."""
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def source(path):
    return (root / path).read_text()


platform = source("src/ui/platform_activity.cpp")
assert "{{xbox_card, ps_card, steam_tile}, {about_tile}}" in platform
assert "auto* steam_tile = makePlatformTile(" in platform
assert '"img/platform/steam.png"' in platform
assert "SteamPlatformMark" not in platform
assert (root / "romfs/img/platform/steam.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
mapping = source("src/input/button_mapping.cpp")
assert 'ButtonMappingProfile::Steam) return "steam_button_mapping"' in mapping
settings = source("src/ui/steam_settings_activity.cpp")
assert '".steam-input"' in settings
assert "steamlink::commitSettingsFile(temporary, path)" in settings
assert "ButtonMappingProfile::Steam" in settings
discard = settings.split('"lunarnx/steam_ui/discard"', 1)[1].split("auto* root", 1)[0]
assert "popActivity" in discard and "closeSettings" not in discard
hosts = source("src/ui/steam_link_activity.cpp")
stream = source("src/ui/stream_view.cpp")
assert "new SteamSettingsActivity" in hosts and "new SteamSettingsActivity" in stream
assert "loadSteamInputSettings()" in hosts
assert "steam->configurePointer(settings.touch, settings.gyro)" in stream
client = source("src/steamlink/steam_link_client.cpp")
discovery = client.split("bool SteamLinkClient::startDiscovery", 1)[1].split(
    "void SteamLinkClient::stopDiscovery", 1)[0]
assert discovery.index("if (discovery_started_)") < discovery.index("IHS_ClientStartDiscovery")
assert "hosts_.clear()" not in discovery and "host_infos_.clear()" not in discovery
ui_sources = "\n".join(p.read_text() for p in (root / "src/ui").glob("*.cpp"))
keys = set(re.findall(r'"lunarnx/steam_ui/([^"\n]+)"', ui_sources))
for locale in ("en-US", "zh-Hans", "zh-Hant"):
    data = json.loads(source(f"romfs/i18n/{locale}/lunarnx.json"))
    assert keys <= data["steam_ui"].keys(), (locale, keys - data["steam_ui"].keys())
    assert all(data["steam_ui"][key].strip() for key in keys)
print("Steam static UI contracts passed (not end-to-end validation)")
