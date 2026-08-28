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
    require("chiaki_holepunch_upnp_discover" in connector and
            "if (err == CHIAKI_ERR_SUCCESS && attempt_profile.discover_upnp)" in connector,
            "UPnP must be available only on the post-failure NAT compatibility path")
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
            "retry_policy.recordStunAllocation(random_allocation)" in connector and
            'on_status("Preparing media channel for restrictive NAT...")' in connector,
            "random CTRL STUN allocation must upgrade the later DATA holepunch")
    require("UPnP is an optional candidate source" in connector and
            "return false" not in connector.split(
                "const ChiakiErrorCode upnp_err", 1)[1].split(
                    "if (err == CHIAKI_ERR_SUCCESS)", 1)[0],
            "missing router UPnP support must not abort STUN traversal")
    require("Ryubing UDP relay is unavailable with Akira Chiaki" in connector,
            "Ryubing profile must fail visibly when using unpatched Akira Chiaki")
    require("kRemoteMaxAttempts = 3" in connector and
            "isRetryableRemoteError" in connector,
            "native remote connector must retry transient full-flow failures")
    require("retry_cv_.wait_for" in connector and
            "retry_cv_.notify_all" in connector,
            "remote retry backoff must be immediately cancellable")
    require("result.failed_phase" in connector and
            "result.error" in connector and
            "remote_result_.failed_phase" in controller and
            "remote_result_.attempts" in controller,
            "remote failures must retain their phase and Chiaki error")
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
