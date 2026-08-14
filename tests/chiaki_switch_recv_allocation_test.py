#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-recv-allocation.patch").read_text()
BUILD = (ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()
AB_BUILD = (ROOT / "tools/chiaki_switch/build_recv_ab.sh").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require("#if LUNARNX_CHIAKI_RECV_OPT" in PATCH,
        "allocation optimization must remain compile-time selectable")
require("takion_handle_packet(takion, buf, received_size);" in PATCH,
        "optimized path must transfer the original receive allocation")
require("realloc(buf, received_size)" in PATCH,
        "control path must preserve the upstream allocation behavior")
require('CHIAKI_RECV_OPT=${CHIAKI_RECV_OPT:-1}' in BUILD,
        "normal Switch SDK builds must default to the optimized path")
require("-DLUNARNX_CHIAKI_RECV_OPT=$CHIAKI_RECV_OPT" in BUILD,
        "Switch SDK build must forward the selected mode to Chiaki")
require("build_variant 0 LunarNX-chiaki-recv-control.nro" in AB_BUILD and
        "build_variant 1 LunarNX-chiaki-recv-optimized.nro" in AB_BUILD,
        "A/B builder must emit both clearly named variants")

print("Chiaki Switch receive allocation A/B test passed")
