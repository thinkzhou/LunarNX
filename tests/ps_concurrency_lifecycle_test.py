#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


login_h = (ROOT / "src/ui/psn_login_activity.h").read_text()
login_cpp = (ROOT / "src/ui/psn_login_activity.cpp").read_text()
auth_h = (ROOT / "src/ps/psn_auth_manager.h").read_text()
auth_cpp = (ROOT / "src/ps/psn_auth_manager.cpp").read_text()
activity = (ROOT / "src/ui/ps_activity.cpp").read_text()
runtime = (ROOT / "src/app/stream_runtime.h").read_text()
view = (ROOT / "src/ui/stream_view.cpp").read_text()
view_h = (ROOT / "src/ui/stream_view.h").read_text()
xbox = (ROOT / "src/app/stream_controller.cpp").read_text()
ps = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
ps_h = (ROOT / "src/ps/ps_stream_controller.h").read_text()
repository_h = (ROOT / "src/ps/ps_console_repository.h").read_text()

require("std::shared_ptr<ps::PsManager>" in login_h and
        "ps::PsnAuthManager& auth_" not in login_h,
        "detached PSN login work must own the PS manager lifetime")
require("alive" in login_cpp.split("submitRedirectUrl", 1)[1],
        "PSN authorization-code exchange must receive page cancellation")
require("CancelCallback cancel" in auth_h.split("submitRedirectUrl", 1)[1] and
        "CancelCallback cancel" in auth_h.split("exchangeCodeForToken", 1)[1],
        "PSN login token exchange must expose cancellation")

require("OperationGeneration" in view_h and "requestStop()" in runtime and
        "CancelCallback cancel" in runtime,
        "stream runtime must expose terminal cancellation to foreground recovery")
stop_body = view.split("void StreamView::stopAndReturn()", 1)[1].split(
    "void StreamView::setQuickMenuVisible", 1)[0]
require("lifecycle_generation_->invalidate()" in stop_body and
        stop_body.index("runtime_->requestStop()") < stop_body.index("startNetworkWorker"),
        "terminal exit must synchronously invalidate and cancel pending recovery")
require("app::IStreamRuntime::CancelCallback cancel" in xbox and
        "startStreamWithProfile(profile, options, std::move(cancel))" in xbox,
        "Xbox foreground rebuild must preserve terminal cancellation")
require("app::IStreamRuntime::CancelCallback cancel" in ps and
        ps.count("if (cancel && cancel())") >= 2,
        "PS foreground rebuild must check terminal cancellation around reset")

require("OperationGeneration request_generation_" in auth_h,
        "PSN auth must invalidate stale network completions")
require("request_generation_.invalidate()" in
        auth_cpp.split("void PsnAuthManager::signOut()", 1)[1],
        "sign-out must invalidate in-flight token and identity responses")
load_token = auth_cpp.split("bool PsnAuthManager::loadToken", 1)[1].split(
    "bool PsnAuthManager::saveToken", 1)[0]
require("request_lock(request_mutex_)" in load_token and
        load_token.index("request_lock(request_mutex_)") < load_token.index("std::fopen") and
        "state_.store(valid ? PsnAuthState::Authenticated" in load_token,
        "token loading must serialize with requests and commit data plus state atomically")
valid_fast_path = auth_cpp.split("if (hasValidToken())", 1)[1].split("return true;", 1)[0]
require("getAccountId" in valid_fast_path and
        "markAuthenticated(ticket)" in valid_fast_path,
        "a valid token may not bypass the account ID required by Remote Play")
identity_worker = activity.split('startNetworkWorker("psn-identity"', 1)[1].split(
    "if (!resumed_once_)", 1)[0]
require("identity_fetching->store(false)" in identity_worker,
        "identity single-flight state must be released after success or failure")
require("file_mutex_" in repository_h,
        "PSN cache save and clear operations must be serialized")
require("last_error_mutex_" in ps_h and
        "std::string lastError() const;" in ps_h and
        "std::lock_guard<std::mutex> lock(last_error_mutex_)" in ps,
        "Chiaki event and UI threads must exchange the controller error safely")

print("PS concurrency lifecycle regression checks passed")
