#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    auth = Path("src/ps/psn_auth_manager.cpp").read_text()
    login_ui = Path("src/ui/psn_login_activity.cpp").read_text()
    login_header = Path("src/ui/psn_login_activity.h").read_text()
    callback_server = Path("src/ps/psn_callback_server.cpp").read_text()
    qr_source = Path("src/ui/qr_code.cpp").read_text()
    ps_ui = Path("src/ui/ps_activity.cpp").read_text()
    manager = Path("src/ps/ps_manager.cpp").read_text()
    repository = Path("src/ps/ps_console_repository.cpp").read_text()
    controller = Path("src/ps/ps_stream_controller.cpp").read_text()
    http = Path("src/api/http_client.cpp").read_text()
    http_header = Path("src/api/http_client.h").read_text()

    require("strnlen(duid_buffer" in auth,
            "DUID must use its NUL terminator instead of chiaki's mutated size")
    require("duid_.assign(duid_buffer, buffer_size)" not in auth,
            "chiaki's DUID buffer size must not become a string length")
    require('"grant_type", "refresh_token"' in auth,
            "expired PSN sessions must use the refresh-token grant")
    require("if (refreshed) *refreshed = false" in auth and
            "if (refreshed) *refreshed = true" in auth,
            "PSN callers must be able to persist only when a refresh occurred")
    require('startNetworkWorker("psn-token-exchange"' in login_ui,
            "phone callback exchange must not block the Borealis UI thread")
    require("PsnCallbackServer" in login_header and "callback_server_.start" in login_ui,
            "PSN login must start a temporary phone callback service")
    require('new QrCodeView(340.0f)' in login_ui,
            "the phone sign-in QR must be large enough to scan from the Switch screen")
    require('"Open Browser"' not in login_ui and "swkbdShow" not in login_ui and
            '"Import from SD Card"' not in login_ui,
            "the normal PSN login UI must only expose the guided phone flow")
    require("nifmGetCurrentIpAddress" in callback_server and
            "bind(fd" in callback_server and "listen(fd" in callback_server,
            "the phone helper must listen on the Switch local network")
    require('locale == "zh-Hans"' in callback_server and
            'locale == "zh-Hant"' in callback_server and
            "getResolvedAppLocale()" in login_ui,
            "the phone helper must follow the resolved LunarNX language")
    require('"&state=" + session_id' in callback_server and
            'input.find("&state=" + session_id' in callback_server,
            "the phone callback must be bound to a one-time OAuth state")
    require("kMaxRequestSize" in callback_server and "kMaxCallbackSize" in callback_server and
            'diagnosticLog("psn-callback", "Sony redirect received length=%zu"' in callback_server,
            "the callback service must bound input and avoid logging the authorization code")
    require("qrcodegen::QrCode::encodeText" in qr_source,
            "OAuth QR codes must use the standards-compliant QR encoder")
    require('startNetworkWorker("psn-device-list"' in ps_ui,
            "PSN device lookup must not block the Borealis UI thread")
    require('lunarnx/ps/search_lan' in ps_ui and 'lunarnx/ps/refresh_psn' in ps_ui,
            "LAN and PSN discovery must have separate manual buttons")
    first_resume = ps_ui.split("if (!resumed_once_)", 1)[1].split("} else {", 1)[0]
    require("refreshConsoles();" not in first_resume,
            "opening the PS page must not automatically start discovery")
    require("getLastPsnStatusCode() == 401" in manager and
            "psn_auth_.refreshToken()" in manager,
            "a PSN 401 must force token refresh before one device-list retry")
    require("token_refreshed && !psn_auth_.saveToken(get_psn_token_path())" in manager,
            "rotated PSN tokens must be persisted after a successful lookup")
    first_token_save = manager.index("psn_auth_.saveToken(get_psn_token_path())")
    first_device_fetch = manager.index("repository_->fetchPsnDevices")
    require(first_token_save < first_device_fetch,
            "a refreshed PSN token must be persisted before device lookup")
    require("getSensitive" in http_header and "http.getSensitive(" in auth and
            "kTokenUrl, headers" in auth,
            "PSN identity lookup must redact its access-token URL from logs")
    require('path + ".tmp"' in auth and 'path + ".bak"' in auth and
            "std::fflush" in auth and "had_existing" in auth,
            "PSN token persistence must replace through a recoverable FAT-compatible backup")
    require("PsnAuthErrorKind::SessionExpired" in auth and
            '"invalid_grant"' in auth and
            "PsnAuthErrorKind::Transient" in auth,
            "PSN refresh failures must distinguish rejected credentials from transport errors")
    require(ps_ui.count("getAuthErrorKind()") >= 2 and
            ps_ui.count("PsnAuthErrorKind::SessionExpired") >= 2,
            "the PS page must only force login when Sony rejects the stored session")
    require("api::HttpClient" in repository and
            "chiaki_holepunch_list_devices" not in repository,
            "PSN device lookup must use LunarNX HTTP timeouts")
    require("platform=PS5" in repository and
            "CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4" not in repository,
            "PSN device listing only supports PS5; PS4 remains LAN-only")
    require('lunarnx/ps/empty_remote' in ps_ui and
            'lunarnx/ps/psn_refresh_done' in ps_ui,
            "an empty PSN account must leave the refreshing state")
    require("account_state_" in ps_ui and "psn_state_" in ps_ui and
            "lan_state_" in ps_ui and "action_status_" in ps_ui,
            "account, discovery, and console action states must stay separate")
    require("lunarnx/ps/pair_ps4" in ps_ui and "lunarnx/ps/pair_ps5" in ps_ui,
            "LAN pairing must remain available when discovery finds nothing")
    require("CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds" in http and
            "CURLOPT_CONNECTTIMEOUT_MS, 0L" not in http,
            "unreachable PSN hosts must not block forever during connect")
    require("remote_result_.psn_account_id" in controller,
            "decoded PSN account ID must reach the remote Chiaki session")

    print("PSN auth flow tests passed")


if __name__ == "__main__":
    main()
