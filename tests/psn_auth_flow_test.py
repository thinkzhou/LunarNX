#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    auth = Path("src/ps/psn_auth_manager.cpp").read_text()
    login_ui = Path("src/ui/psn_login_activity.cpp").read_text()
    qr_source = Path("src/ui/qr_code.cpp").read_text()
    ps_ui = Path("src/ui/ps_activity.cpp").read_text()
    manager = Path("src/ps/ps_manager.cpp").read_text()
    repository = Path("src/ps/ps_console_repository.cpp").read_text()
    controller = Path("src/ps/ps_stream_controller.cpp").read_text()
    http = Path("src/api/http_client.cpp").read_text()

    require("strnlen(duid_buffer" in auth,
            "DUID must use its NUL terminator instead of chiaki's mutated size")
    require("duid_.assign(duid_buffer, buffer_size)" not in auth,
            "chiaki's DUID buffer size must not become a string length")
    require("webConfigSetCallbackUrl" in auth and "WebExitReason_LastUrl" in auth,
            "Switch browser login must capture Sony's callback URL")
    require('"grant_type", "refresh_token"' in auth,
            "expired PSN sessions must use the refresh-token grant")
    require('startNetworkWorker("psn-token-exchange"' in login_ui,
            "manual code exchange must not block the Borealis UI thread")
    require('callback_input->init(' in login_ui and '"Enter Code / URL"' in login_ui,
            "manual callback entry must be visible as an explicit input field")
    require('"Import from SD Card"' in login_ui and 'get_psn_callback_import_path()' in login_ui,
            "simulator callback import must not depend on the Switch keyboard applet")
    require("qrcodegen::QrCode::encodeText" in qr_source,
            "OAuth QR codes must use the standards-compliant QR encoder")
    require('startNetworkWorker("psn-device-list"' in ps_ui,
            "PSN device lookup must not block the Borealis UI thread")
    require('setText("Search LAN")' in ps_ui and 'setText("Refresh PSN")' in ps_ui,
            "LAN and PSN discovery must have separate manual buttons")
    first_resume = ps_ui.split("if (!resumed_once_)", 1)[1].split("} else {", 1)[0]
    require("refreshConsoles();" not in first_resume,
            "opening the PS page must not automatically start discovery")
    require("getLastPsnStatusCode() == 401" in manager and
            "psn_auth_.refreshToken()" in manager,
            "a PSN 401 must force token refresh before one device-list retry")
    require("psn_auth_.saveToken(get_psn_token_path())" in manager,
            "rotated PSN tokens must be persisted after a successful lookup")
    require("api::HttpClient" in repository and
            "chiaki_holepunch_list_devices" not in repository,
            "PSN device lookup must use LunarNX HTTP timeouts")
    require("platform=PS5" in repository and
            "CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4" not in repository,
            "PSN device listing only supports PS5; PS4 remains LAN-only")
    require('"No remote PS5 consoles loaded"' in ps_ui and
            '"PSN: Refresh complete (PS5 only)"' in ps_ui,
            "an empty PSN account must leave the refreshing state")
    require("account_state_" in ps_ui and "psn_state_" in ps_ui and
            "lan_state_" in ps_ui and "action_status_" in ps_ui,
            "account, discovery, and console action states must stay separate")
    require("Pair PS4 by IP" in ps_ui and "Pair PS5 by IP" in ps_ui,
            "LAN pairing must remain available when discovery finds nothing")
    require("CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds" in http and
            "CURLOPT_CONNECTTIMEOUT_MS, 0L" not in http,
            "unreachable PSN hosts must not block forever during connect")
    require("remote_result_.psn_account_id" in controller,
            "decoded PSN account ID must reach the remote Chiaki session")

    print("PSN auth flow tests passed")


if __name__ == "__main__":
    main()
