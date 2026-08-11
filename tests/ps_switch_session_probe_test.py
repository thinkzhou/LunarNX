#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "tools/psn_remote_probe_native.c").read_text()
MAKEFILE = (ROOT / "Makefile.switch.psprobe").read_text()
RUNNER = (ROOT / "tools/ps_udp_relay/run_ryubing_probe.sh").read_text()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


require("psn_remote_probe_native" in MAKEFILE,
        "Switch PS probe must use the same full-flow source as the Mac gate")
require("src/tools/switch_ps_session_probe.c" not in MAKEFILE,
        "obsolete session-init-only probe must not be built")
require("#ifdef __SWITCH__" in SOURCE,
        "full-flow probe must provide a Switch entry path")
require('sdmc:/switch/LunarNX/psn_token.json' in SOURCE,
        "Switch probe must load the existing PSN token")
require('sdmc:/switch/LunarNX/config.json' in SOURCE,
        "Switch probe must load the emulator network profile")
require('"ps_network_profile"' in SOURCE and '"ryubing"' in SOURCE,
        "Switch probe must refuse relay mode outside the Ryubing profile")
require("socketInitialize" in SOURCE,
        "Switch probe must initialize guest networking itself")
require("chiaki_holepunch_session_set_port_guessing_socks(holepunch, 64)" in SOURCE,
        "Switch probe must match the active Ryubing candidate configuration")
require('trace_phase("upnp_discovery", "skipped"' in SOURCE,
        "Switch probe must match the UI relay path and skip native UPnP setup")
require("PROBE_RESULT" in SOURCE,
        "Switch probe must expose a machine-readable terminal result")
require("chiaki_session_fini(&session);\n    chiaki_session_fini(&session);" not in SOURCE,
        "probe must not finalize the Chiaki session twice")
require("open -na" in RUNNER and "--disable-file-logging" in RUNNER,
        "runner must use the documented LaunchServices Ryubing launch")
require("LunarNXPsSessionProbe.nro" in RUNNER,
        "runner must launch the UI-free PS probe NRO")
require("PROBE_RESULT" in RUNNER,
        "runner must wait for the machine-readable probe result")
require("refresh_psn_token.py" in RUNNER,
        "runner must refresh expired PSN credentials without UI interaction")
require('rm -f "$LOG"' in RUNNER and RUNNER.index('rm -f "$LOG"') < RUNNER.index("open -na"),
        "runner must discard only the stale generated result before launch")
require('"$attempt" -le 4' in RUNNER and 'kill "$probe_pid"' in RUNNER,
        "runner must retry and clean up the UI-free probe process automatically")

print("PS Switch session probe tests passed")
