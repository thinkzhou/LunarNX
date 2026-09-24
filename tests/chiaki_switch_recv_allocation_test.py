#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-recv-allocation.patch").read_text()
BUILD = (ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()
CONTAINER_BUILD = (ROOT / "tools/chiaki_switch/build_in_container.sh").read_text()
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
require("CHIAKI_RECV_OPT" not in BUILD and
        "lunarnx-chiaki-recv-allocation.patch" not in CONTAINER_BUILD,
        "the legacy receive-allocation experiment must not appear active in v15")
require("no longer available" in AB_BUILD,
        "the legacy A/B builder must not emit identical v15 artifacts")

print("Chiaki Switch receive allocation archival test passed")
