#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    ui = Path("src/ui/ps_activity.cpp").read_text()
    discovery = Path("src/ps/ps_discovery.cpp").read_text()

    require('lunarnx/ps/tab_local' in ui and
            'lunarnx/ps/tab_remote' in ui and
            "PsConsoleSource::Local" in ui and "PsConsoleSource::Remote" in ui,
            "PS page must use Xbox-style local and remote source tabs")
    require('return makeAppFrame("PlayStation", workspace);' in ui and
            'makeSidebarButton(brls::getStr("lunarnx/ps/tab_local"), true)' in ui and
            'makeSidebarButton(brls::getStr("lunarnx/ps/tab_remote"))' in ui and
            'makeSidebarButton(brls::getStr("lunarnx/ps/pair_by_ip"))' in ui and
            'lunarnx/ps/account_network' in ui,
            "PS page must use the platform workspace shell and explicit source navigation")
    require('lunarnx/common/settings' in ui and
            'new PsSettingsActivity(' in ui and
            'lunarnx/common/about' in ui and
            'new AboutActivity()' in ui,
            "PS page must expose the shared settings and about activities")
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
    require("card->setHeight(132);" in ui and
            "name->setFontSize(23);" in ui and
            "local_console_desc" in ui and "remote_console_desc" in ui,
            "dynamic PS console rows must match Xbox card dimensions and typography")
    require('detail_remote_disabled' in ui and 'btn_how_enable' in ui,
            "remote-disabled devices must remain visible with guidance")
    require('lunarnx/ps/pair_ps4' in ui and 'lunarnx/ps/pair_ps5' in ui,
            "manual local pairing must remain available")
    require('card->setFocusable(true)' in ui and
            'card->registerClickAction' in ui and
            'action->setText(action_text)' in ui,
            "available PS hosts must use whole-card focus with a visible action label")
    require('refreshConsoles();' not in ui.split('if (!resumed_once_)', 1)[1].split('} else {', 1)[0],
            "opening the page must not auto-trigger network discovery")
    require("chiaki_discovery_service_init" in discovery,
            "LAN search must actively send discovery packets")

    print("PS page flow tests passed")


if __name__ == "__main__":
    main()
