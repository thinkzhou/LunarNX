#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def ordered(source, markers):
    positions = [source.find(marker) for marker in markers]
    return all(position >= 0 for position in positions) and positions == sorted(positions)


def main():
    connector = Path("src/ps/ps_remote_connector.cpp").read_text()
    retry_policy = Path("src/ps/ps_remote_retry_policy.h").read_text()
    connector_header = Path("src/ps/ps_remote_connector.h").read_text()
    auth_manager = Path("src/ps/psn_auth_manager.cpp").read_text()
    controller = Path("src/ps/ps_stream_controller.cpp").read_text()
    controller_header = Path("src/ps/ps_stream_controller.h").read_text()
    session = Path("src/ps/ps_stream_session.cpp").read_text()
    stop_body = session.split("void PsStreamSession::stop()", 1)[1].split(
        "void PsStreamSession::setLoginPin", 1)[0]

    require(ordered(connector, [
        "chiaki_holepunch_session_init",
        "chiaki_holepunch_session_create(session)",
        "holepunch_session_create_offer(session)",
        "chiaki_holepunch_session_start",
        "CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL",
    ]), "remote connector must follow chiaki-ng control-hole ordering")
    require("chiaki_holepunch_upnp_discover" not in connector and
            "discover_upnp" not in retry_policy,
            "session-owned UPnP probing must not make loading-page cancellation unbounded")
    require("CHIAKI_HOLEPUNCH_PORT_TYPE_DATA" not in connector,
            "remote connector must leave the data hole to ChiakiSession")
    require("chiaki_get_holepunch_sock" not in connector,
            "remote connector must not extract control or data sockets")
    require("ctrl_sock" not in connector_header and
            "data_sock" not in connector_header and
            "data_sock_for_connect" not in connector_header,
            "remote result must transfer only the holepunch session")
    require("connect_info_.rudp_sock" not in session,
            "remote session must not inject a data socket as RUDP")
    require("chiaki_holepunch_session_fini" not in stop_body,
            "ChiakiSession must be the sole owner after handoff")
    require("chiaki_holepunch_main_thread_cancel" in connector,
            "connector cancellation must interrupt holepunch work")
    require('"ps_network_profile"' in connector and
            '"ryubing"' in connector and
            '"native_switch"' in connector,
            "remote connector must select an explicit runtime network profile")
    require("chiaki_holepunch_session_set_port_guessing_socks" in connector,
            "remote connector must apply the configured NAT probe socket count")
    require("kPsFastNatSockets = 64" in retry_policy and
            "chiaki_holepunch_session_force_port_guessing(session, true)" in connector,
            "native Switch must retain bounded port-rewriting NAT traversal")
    require("kPsCompatibilityNatSockets = 120" in retry_policy and
            "kPsCompatibilityPortGuesses = 96" in retry_policy and
            'phase == "control punch"' in retry_policy,
            "CTRL candidate failure must enable a bounded wider NAT fallback")
    require("chiaki_holepunch_session_get_stun_allocation" in connector and
            "retry_policy.recordStunAllocation(random_allocation)" in connector,
            "remote diagnostics must retain the observed CTRL STUN allocation")
    require("PsRoutePreferenceStore().load" in connector and
            "chiaki_holepunch_session_set_preferred_stun_server" in connector and
            "chiaki_holepunch_session_set_preferred_remote_candidate" in connector,
            "PS remote startup must apply soft route preferences")
    require('on_status("Preparing media channel for restrictive NAT...")' not in connector and
            '"data-nat-policy", "upgraded"' not in connector,
            "a successful CTRL offer must retain the v0.2 DATA holepunch parameters")
    require("Ryubing UDP relay is unavailable with Akira Chiaki" in connector,
            "Ryubing profile must fail visibly when using unpatched Akira Chiaki")
    require("kRemoteMaxAttempts = 3" in connector and
            "isRetryableRemoteError" in connector,
            "native remote connector must retry transient full-flow failures")
    retry_classifier = connector.split(
        "bool isRetryableRemoteError", 1)[1].split("} // namespace", 1)[0]
    require("CHIAKI_ERR_HTTP_NONOK" not in retry_classifier and
            "CHIAKI_ERR_INVALID_RESPONSE" not in retry_classifier and
            "CHIAKI_ERR_UNKNOWN" not in retry_classifier,
            "deterministic HTTP, protocol and unknown failures must not be retried blindly")
    require("TokenRefreshCallback" in connector_header and
            "auth_refresh_attempted" in connector and
            "CHIAKI_ERR_HTTP_NONOK" in connector and
            "chiaki_holepunch_session_get_last_http_status" in connector and
            "refresh_token" in connector and
            "shouldRefreshPsnToken" in connector,
            "an explicit 401/403 PSN rejection must get exactly one refresh path")
    require("retry_cv_.wait_for" in connector and
            "retry_cv_.notify_all" in connector,
            "remote retry backoff must be immediately cancellable")
    require("result.failed_phase" in connector and
            "result.error" in connector and
            "result.token_refresh_failed" in connector and
            "lastPsnRefreshFailed" in controller_header and
            "lastPsnRefreshSessionExpired" in controller_header and
            "remote_result_.failed_phase" in controller and
            "remote_result_.attempts" in controller,
            "remote failures must retain their phase, Chiaki error and refresh provenance")
    require("ChiakiLog remote_log_{}" in controller_header and
            "&remote_log_" in controller and
            "auto log = makeChiakiDiagnosticLog" not in controller,
            "holepunch session logger must outlive asynchronous remote streaming")
    require("kRemoteSessionTokenMarginMs = 10 * 60 * 1000" in auth_manager and
            "nowMilliseconds() + kRemoteSessionTokenMarginMs" in auth_manager,
            "PSN tokens must retain enough lifetime for all remote retry attempts")
    require("setPsnCredentials" in controller_header and
            "state_.load() != app::StreamState::Idle" in controller,
            "refreshed PSN credentials must only be applied before stream startup")
    require("CHIAKI_EVENT_HOLEPUNCH" in session,
            "session must surface Chiaki-owned data-hole progress")
    require("connect_info_.morning" in session and
            "plan_.credentials->rp_key" in controller,
            "LAN sessions must pass both Akira registration keys to Chiaki")
    require("CHIAKI_EVENT_LOGIN_PIN_REQUEST" in session and
            "PIN required - re-register the console" not in session,
            "console login PIN must be interactive, not a pairing error")

    print("PS remote flow tests passed")


if __name__ == "__main__":
    main()
