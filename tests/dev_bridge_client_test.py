#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


client = (ROOT / "src/app/dev_bridge_client.cpp").read_text()
client_header = (ROOT / "src/app/dev_bridge_client.h").read_text()
ui = (ROOT / "src/ui/dev_tools_activity.cpp").read_text()
platform = (ROOT / "src/ui/platform_activity.cpp").read_text()
makefile = (ROOT / "Makefile.switch").read_text()

require("/dev/versions.json" in client and "cJSON_ArrayForEach" in client,
        "development client must parse the version index")
require("25LL * 1024LL * 1024LL" in client,
        "updater must enforce the Workers KV object limit")
require("mbedtls_sha256" in client and "build.sha256" in client,
        "downloaded NRO must be verified with SHA-256")
require("CURLOPT_XFERINFOFUNCTION" in client and "ProgressCallback" in client_header,
        "streaming download must report progress")
require(client.count("LUNARNX_CURL_VERIFY_SSL ? 1L : 0L") >= 2 and
        client.count("LUNARNX_CURL_VERIFY_SSL ? 2L : 0L") >= 2,
        "downloads and log uploads must follow the Switch TLS build setting")
require('target_path + ".update"' in client and 'target + ".backup"' in client and
        'backup + ".previous"' in client,
        "installation must preserve temporary, backup, and rollback archive files")
require("/dev/logs" in client and "LUNARNX_DEV_UPLOAD_TOKEN" in client,
        "log upload must use the authenticated bridge endpoint")
require("X-LunarNX-Commit" in client and "X-LunarNX-Device" in client and
        'jsonString(root, "log_id")' in client,
        "log upload must identify its build and return the server log ID")
require("envSetNextLoad" in ui and "Application::quit" in ui,
        "successful installation must relaunch the installed NRO")
require("new DevToolsActivity" in platform,
        "platform page must expose development tools without account login")
require('makeAppFrame(brls::getStr("lunarnx/dev/title"), scroll)' in ui and
        'title->setText(brls::getStr("lunarnx/dev/title"))' not in ui and
        'card->setHeight(92)' in ui,
        "development tools must use the shared title and compact card hierarchy")
require("src/app/dev_bridge_client.cpp" in makefile and
        "src/ui/dev_tools_activity.cpp" in makefile and
        "DEV_BRIDGE_UPLOAD_TOKEN" in makefile,
        "Switch build must include the client, UI, and injected upload token")
require("lunarnx.tooyang.qzz.io" in client_header,
        "client must use the custom-domain development bridge")
require("Historical manifests contain the old workers.dev origin" in client and
        "build.download_url = std::string(DevBridgeClient::kBaseUrl)" in client,
        "historical manifests must download through the custom domain")

print("Development bridge client regression checks passed")
