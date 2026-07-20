#!/usr/bin/env python3
import struct
import sys
from pathlib import Path


MAX_BSS_SIZE = 32 * 1024 * 1024
NRO_BSS_SIZE_OFFSET = 0x38


def main():
    nro_path = Path(sys.argv[1] if len(sys.argv) > 1 else "build/switch/LunarNX.nro")
    data = nro_path.read_bytes()

    if len(data) < NRO_BSS_SIZE_OFFSET + 4:
        raise SystemExit(f"FAIL: {nro_path} is too small to contain an NRO header")

    bss_size = struct.unpack_from("<I", data, NRO_BSS_SIZE_OFFSET)[0]
    if bss_size > MAX_BSS_SIZE:
        raise SystemExit(
            f"FAIL: NRO BSS is {bss_size / 1024 / 1024:.1f} MiB; "
            f"expected at most {MAX_BSS_SIZE / 1024 / 1024:.0f} MiB"
        )

    print(f"Switch NRO BSS test passed ({bss_size / 1024 / 1024:.1f} MiB)")


if __name__ == "__main__":
    main()
