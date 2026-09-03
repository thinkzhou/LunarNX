#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


auth_header = (ROOT / "src/auth/auth_manager.h").read_text()
auth_source = (ROOT / "src/auth/auth_manager.cpp").read_text()
session_header = (ROOT / "src/app/xbox_stream_session.h").read_text()
session_source = (ROOT / "src/app/xbox_stream_session.cpp").read_text()
controller = (ROOT / "src/app/stream_controller.cpp").read_text()
http_header = (ROOT / "src/api/http_client.h").read_text()
http_source = (ROOT / "src/api/http_client.cpp").read_text()

require(
    "using CancelCallback = api::HttpClient::CancelCallback;" in auth_header
    and "bool refreshTokensIfNeeded(CancelCallback cancel = {});" in auth_header
    and "bool refreshStreamingTokens(bool force = false," in auth_header,
    "streaming token refresh must expose an HTTP cancellation callback",
)

refresh_start = auth_source.index("bool AuthManager::refreshTokensIfNeeded(")
refresh_end = auth_source.index("bool AuthManager::shouldRefreshTokens()", refresh_start)
refresh = auth_source[refresh_start:refresh_end]
derive_start = auth_source.index("bool AuthManager::stepGetStreamingTokens(")
derive_end = auth_source.index("bool AuthManager::fetchStreamToken(", derive_start)
derive = auth_source[derive_start:derive_end]
fetch_start = derive_end
fetch_end = auth_source.index("bool AuthManager::loadTokens(", fetch_start)
fetch = auth_source[fetch_start:fetch_end]

require(
    "http_.post(TOKEN_URL, body.str(), headers, cancel)" in refresh
    and "stepGetStreamingTokens(cancel)" in refresh,
    "MSAL refresh and Xbox token derivation must receive stream-stop cancellation",
)
require(
    derive.count(", cancel);\n") >= 3
    and derive.count("false, cancel)") >= 2
    and "true, cancel)" in derive,
    "every RPS/XSTS/GSSV request in token derivation must be cancellable",
)
require(
    "http_.post(login_url, gstr, gssv_headers, cancel)" in fetch,
    "GSSV token fetch must pass cancellation into curl",
)

require(
    "std::function<bool(bool force, CancelCallback cancel)> refresh_tokens;"
    in session_header,
    "Xbox runtime callbacks must carry cancellation into token refresh",
)
control_start = session_source.index("void XboxStreamSession::controlLoop(")
control_end = session_source.index("void XboxStreamSession::cleanupResources(", control_start)
control = session_source[control_start:control_end]
require(
    "callbacks.refresh_tokens(true, cancel)" in control
    and "callbacks.refresh_tokens(false, cancel)" in control,
    "control-loop refresh calls must observe stream shutdown",
)
require(
    "auth_ref.refreshStreamingTokens(true, cancel)" in controller
    and "auth_ref.refreshTokensIfNeeded(cancel)" in controller,
    "StreamController must forward the control-loop cancellation callback",
)
require(
    "getXcloudTransferToken(bool force_refresh = false," in auth_header
    and "http_.post(LIVE_TOKEN_URL, body.str(), headers, cancel)" in auth_source
    and "auth_ref.refreshTokensIfNeeded(stream_cancel)" in controller
    and "auth_ref.getXcloudTransferToken(true, stream_cancel)" in controller
    and "callbacks.external_cancel = stream_cancel;" in controller,
    "connection startup and xCloud transfer-token HTTP must share stop cancellation",
)

require(
    "std::timed_mutex mutex_;" in http_header
    and "lockUnlessCancelled" in http_source
    and http_source.count("lockUnlessCancelled(lock, cancel") >= 3,
    "HttpClient lock acquisition itself must be cancellable",
)

print("Xbox stream exit cancellation tests passed")
