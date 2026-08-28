#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    ui = Path("src/ui/ps_activity.cpp").read_text()
    discovery = Path("src/ps/ps_discovery.cpp").read_text()
    manager = Path("src/ps/ps_manager.cpp").read_text()

    require('lunarnx/ps/tab_local' in ui and
            'lunarnx/ps/tab_remote' in ui and
            "PsConsoleSource::Local" in ui and "PsConsoleSource::Remote" in ui,
            "PS page must use Xbox-style local and remote source tabs")
    require('return makeAppFrame("PlayStation", workspace);' in ui and
            'UiIcon::Console' in ui and 'UiIcon::Cloud' in ui and
            'UiIcon::Link' in ui and
            'lunarnx/ps/account_network' in ui,
            "PS page must use the platform workspace shell and explicit source navigation")
    require('lunarnx/common/playstation_settings' in ui and
            'new PsSettingsActivity(' in ui and
            'lunarnx/common/about' in ui and
            'new AboutActivity()' in ui,
            "PS page must expose scoped settings and the shared About activity")
    require('back_navigation_ready_at_' in ui and
            'now < back_navigation_ready_at_' in ui,
            "PS page must consume a held Back input after returning from streaming")
    require('remote_title->setFontSize(25)' in ui and
            'local_title->setFontSize(25)' in ui and
            'stylePrimaryButton(psn_button_)' in ui and
            'stylePrimaryButton(lan_button_)' in ui,
            "source headers and refresh actions must match the Xbox content hierarchy")
    require('lunarnx/ps/refresh_psn' in ui and 'lunarnx/ps/search_lan' in ui,
            "PSN refresh and LAN search must remain separate manual actions")
    require('lunarnx/ps/btn_pair' in ui and 'lunarnx/ps/btn_wake_connect' in ui and
            'lunarnx/ps/btn_connect' in ui,
            "console cards must expose truthful goal-oriented actions")
    require("card->setHeight(92);" in ui and
            "name->setFontSize(20);" in ui and
            "name->setSingleLine(true);" in ui and
            "meta->setHeight(28);" in ui and
            "local_console_desc" not in ui and "remote_console_desc" not in ui,
            "dynamic PS console rows must use the compact two-line card hierarchy")
    require('detail_remote_disabled' in ui and 'btn_how_enable' in ui,
            "remote-disabled devices must remain visible with guidance")
    require('lunarnx/ps/pair_ps4' in ui and 'lunarnx/ps/pair_ps5' in ui,
            "manual local pairing must remain available")
    require('card->addView(action)' in ui and
            'action->registerClickAction' in ui and
            'stylePrimaryButton(action)' in ui,
            "available PS hosts must expose focused action buttons without changing flow")
    require('refreshConsoles();' not in ui.split('if (!resumed_once_)', 1)[1].split('} else {', 1)[0],
            "opening the page must not auto-trigger network discovery")
    require("chiaki_discovery_service_init" in discovery,
            "LAN search must actively send discovery packets")
    wakeup = manager.split("void PsManager::wakeupHost", 1)[1].split(
        "PsConnectionPlan PsManager::planConnection", 1)[0]
    require("strtoull" in wakeup and
            "cred->rp_regist_key" in wakeup and
            "memcpy(&user_credential" not in wakeup,
            "PS wakeup must parse the textual registration key as hexadecimal")
    require("pending_wake_mac_" in ui and
            "PsConsoleState::Ready" in ui and
            "connectToConsole(host)" in ui and
            "std::chrono::seconds(25)" in ui,
            "Wake & Connect must wait for the same console to become ready and time out")

    print("PS page flow tests passed")


if __name__ == "__main__":
    main()
