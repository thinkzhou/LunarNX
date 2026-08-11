#!/usr/bin/env python3
from pathlib import Path


SOURCE = Path("src/platform/switch_posix_stubs.c").read_text()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


require("randomGet(output, len);" in SOURCE,
        "Switch entropy must retain libnx's kernel CSPRNG")
require("armGetSystemTick()" in SOURCE,
        "Ryujinx entropy must include a per-launch timing salt")
require("__atomic_add_fetch" in SOURCE,
        "rapid entropy requests must receive distinct process-local salts")
require("output[offset] ^=" in SOURCE,
        "the timing salt must be mixed without replacing CSPRNG output")

print("Switch entropy salt tests passed")
