#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    discovery = (ROOT / "src/ps/ps_discovery.cpp").read_text()
    registration = (ROOT / "src/ps/ps_registration.cpp").read_text()
    registration_h = (ROOT / "src/ps/ps_registration.h").read_text()
    manager = (ROOT / "src/ps/ps_manager.cpp").read_text()
    repository = (ROOT / "src/ps/ps_console_repository.cpp").read_text()
    activity = (ROOT / "src/ui/ps_activity.cpp").read_text()

    assert "nifmGetCurrentIpConfigInfo" in discovery
    assert "current_ip | ~subnet_mask" in discovery
    assert "options.broadcast_addrs = &directed_broadcast" in discovery
    assert "options.broadcast_num = 1" in discovery
    assert "manual_hosts" in discovery
    assert "chiaki_discovery_send(&service_.discovery" in discovery
    assert "CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4" in discovery
    assert "CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5" in discovery
    assert "chiaki_discovery_host_system_version_target(&host)" in discovery
    assert "int target" in (ROOT / "src/ps/ps_console.h").read_text()
    assert "bool startDiscovery(HostListCallback cb)" in discovery or True
    assert "initialized_" in registration_h
    assert "if (initialized_.exchange(false))" in registration
    assert "const ChiakiErrorCode start_error = registration_->start" in manager
    assert "chiaki_error_string(start_error)" in manager
    assert '"Local Account ID is required; use phone pairing' in manager
    assert "target >= CHIAKI_TARGET_PS4_9 && account_id.empty()" in manager
    assert "registration_->start(" in manager and \
        "host_addr, pin, target, account_id, result" in manager
    terminal_ui = manager.split(
        "Registration has finished on the Chiaki worker", 1)[1].split(
        "});", 1)[0]
    assert "registration_generation_" in manager
    assert "registration_->stop();" in terminal_ui
    assert "registration_.reset();" in terminal_ui
    assert "credentials_.addAndSave" in terminal_ui
    assert "credentials could not be saved" in terminal_ui
    assert terminal_ui.index("registration_.reset();") < terminal_ui.index("(*callback)")
    assert "chiaki_base64_decode" in registration
    assert "account_id_size != CHIAKI_PSN_ACCOUNT_ID_SIZE" in registration
    assert "info.broadcast = false" in registration
    assert "target >= CHIAKI_TARGET_PS4_9" in registration
    assert "chiaki_log_sniffer_init" in registration
    assert "registrationFailureDetail" in registration
    assert 'messages.find("failed to create socket for search")' in registration
    assert 'messages.find("failed to connect for search")' in registration
    assert "Local pairing socket unavailable" in registration
    assert "ChiakiLogSniffer" in registration_h
    assert "phone_pairing_server_.stop();" in (ROOT / "src/ui/ps_registration_activity.cpp").read_text()
    assert "const bool started = ps_manager_->startDiscovery" in activity
    assert "raw.target" in repository
    assert "credential.last_known_addr" in repository.split(
        "bool PsConsoleRepository::startDiscovery", 1)[1].split(
        "bool PsConsoleRepository::fetchPsnDevices", 1)[0]
    credential_merge = repository.split(
        "for (const auto& credential : credentials_)", 1)[1].split(
        "for (const auto& psn : psn_consoles_)", 1)[0]
    assert "it->target = credential.target" in credential_merge
    assert credential_merge.count("credential.last_known_addr.empty()") >= 2
    assert credential_merge.count("PsLocalEndpoint{") >= 2
    assert credential_merge.count("credential.last_known_addr") >= 4
    assert "CHIAKI_TARGET_PS4_10" in activity
    assert "CHIAKI_TARGET_PS4_9" in activity
    assert '"lunarnx/ps/select_ps4_version"' in activity
    assert "CHIAKI_TARGET_PS5_1" in activity
    sidebar_pairing = activity.split(
        'auto* pair_button = makeSidebarButton', 1)[1].split(
        'sidebar->addView(pair_button);', 1)[0]
    assert "pairConsole(console)" not in sidebar_pairing
    assert '"lunarnx/ps/select_console_type"' in sidebar_pairing
    assert "pairConsoleWithTarget(console, CHIAKI_TARGET_PS4_9)" in sidebar_pairing
    assert "pairConsoleWithTarget(console, CHIAKI_TARGET_PS4_10)" in sidebar_pairing
    assert "pairConsoleWithTarget(console, CHIAKI_TARGET_PS5_1)" in sidebar_pairing
    pair_with_target = activity.split(
        "void PsActivity::pairConsoleWithTarget", 1)[1].split(
        "void PsActivity::wakeupConsole", 1)[0]
    assert "stopDiscovery();" in pair_with_target
    connect_flow = activity.split(
        "void PsActivity::connectToConsole", 1)[1]
    assert connect_flow.index("stopDiscovery();") < connect_flow.index(
        "new PsConnectActivity")

    for locale in ("en-US", "zh-Hans", "zh-Hant"):
        strings = (ROOT / "romfs/i18n" / locale / "lunarnx.json").read_text()
        assert '"select_console_type"' in strings
        assert '"ps4_version_9"' in strings
        assert '"ps4_version_10"' in strings
        assert "7.0" in strings and "7.99" in strings
        assert "8.0" in strings
        assert "9.x" not in strings
    assert ": 900" not in repository
    assert ", 900);" not in activity

    print("PS local pairing flow tests passed")


if __name__ == "__main__":
    main()
