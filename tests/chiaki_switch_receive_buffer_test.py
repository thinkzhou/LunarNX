#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-recvbuf.patch").read_text()
BUILD = (ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require("#define TAKION_A_RWND 0x19000" in PATCH,
        "the protocol receive window must remain unchanged")
require("#define TAKION_SOCKET_RCVBUF (512 * 1024)" in PATCH,
        "Switch Takion sockets must request a 512 KiB receive buffer")
require(PATCH.count("const int rcvbuf_val = TAKION_SOCKET_RCVBUF;") == 2,
        "both preconnected and locally-created Takion sockets must use the larger buffer")
require(
    'git -C "$src" apply /work/tools/chiaki_switch/lunarnx-chiaki-recvbuf.patch'
    in BUILD,
    "the reproducible Chiaki build must apply the receive-buffer patch",
)

print("chiaki Switch receive buffer regression passed")
