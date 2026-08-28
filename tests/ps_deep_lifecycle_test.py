#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


manager_h = (ROOT / "src/ps/ps_manager.h").read_text()
manager = (ROOT / "src/ps/ps_manager.cpp").read_text()
repository_h = (ROOT / "src/ps/ps_console_repository.h").read_text()
repository = (ROOT / "src/ps/ps_console_repository.cpp").read_text()
auth = (ROOT / "src/ps/psn_auth_manager.cpp").read_text()
http_patch = (
    ROOT / "tools/chiaki_switch/lunarnx-chiaki-http-status.patch"
).read_text()


require(
    "OperationGeneration psn_device_generation_" in manager_h,
    "PSN device work needs an account-generation guard",
)
fetch_manager = manager.split("bool PsManager::fetchPsnDevices", 1)[1].split(
    "bool PsManager::loadPsnDeviceCache", 1
)[0]
require(
    "psn_device_generation_.invalidate()" in fetch_manager
    and "psn_device_generation_.isCurrent" in fetch_manager,
    "a newer device fetch or sign-out must invalidate old completion work",
)
require(
    "CancelCallback" in repository_h
    and "http.get(url" in repository
    and "cancel" in repository.split("http.get(url", 1)[1].split(");", 1)[0],
    "PSN device HTTP must receive the generation cancellation callback",
)
clear_manager = manager.split("void PsManager::clearPsnDeviceCache", 1)[1].split(
    "bool PsManager::loadCredentials", 1
)[0]
require(
    "psn_device_generation_.invalidate()" in clear_manager,
    "sign-out cache clearing must invalidate in-flight old-account fetches",
)
require(
    "savePsnCache(const std::string& account_id," in repository_h
    and "CancelCallback cancel" in repository_h,
    "cache persistence must recheck account generation while serialized with clear",
)
require(
    "psn_consoles_.size()" not in repository.split(
        'diagnosticLog("ps-console-repository", "PSN device list complete', 1
    )[1].split("mergeAndNotify", 1)[0],
    "PSN device diagnostics must not read the shared console vector outside its lock",
)
require(
    'diagnosticLog("ps-manager", "PSN console cache save failed")' in fetch_manager
    and 'psn_device_error_ = "PSN console list could not be saved"' not in fetch_manager,
    "a cache-only write failure must not reject a successful live PSN device fetch",
)

token_commit = auth.split("access_token_ = std::move(access_token);", 1)[1].split(
    "expires_in_ = expires_in", 1
)[0]
require(
    "account_id_.clear()" in token_commit and "online_id_.clear()" in token_commit,
    "authorization-code tokens must not retain identity from the previous account",
)
request_token = auth.split("bool PsnAuthManager::requestToken", 1)[1].split(
    "bool PsnAuthManager::exchangeCodeForToken", 1
)[0]
require(
    request_token.count("request_cancel()") >= 2,
    "token responses must recheck external cancellation before committing credentials",
)

require(
    "record_last_http_status" in http_patch
    and http_patch.count("record_last_http_status(session, curl);") >= 8,
    "all session HTTP phases that can return HTTP_NONOK must expose their status",
)
require(
    "atomic_long last_http_status" in http_patch
    and "atomic_load_explicit" in http_patch
    and http_patch.count("atomic_store_explicit") >= 3,
    "HTTP status shared by the websocket and connector threads must be atomic",
)
for function in (
    "get_websocket_fqdn",
    "http_create_session",
    "http_check_session",
    "http_ps4_session_wakeup",
    "http_start_session",
    "http_send_session_message",
):
    require(
        function in http_patch,
        f"HTTP status patch must cover {function}",
    )

print("PS deep lifecycle regression checks passed")
