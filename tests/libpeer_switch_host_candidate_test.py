#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PORTS_SOURCE = ROOT / "lib/libpeer/src/ports.c"
TRACKED_PATCH = ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch"


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"{label} missing {needle!r}")


def validate_switch_host_candidate(text: str, label: str, require_getifaddrs: bool) -> None:
    require(text, "#ifdef __SWITCH__", label)
    require(text, "u32 current_addr = 0;", label)
    require(text, "Result rc = nifmGetCurrentIpAddress(&current_addr);", label)
    require(text, "R_SUCCEEDED(rc) && current_addr != 0", label)
    require(text, "addr->sin.sin_addr.s_addr = current_addr;", label)
    if require_getifaddrs:
        require(text, "getifaddrs(&ifaddr)", label)


def main() -> None:
    validate_switch_host_candidate(
        PORTS_SOURCE.read_text(), "legacy libpeer ports.c", require_getifaddrs=True
    )
    validate_switch_host_candidate(
        TRACKED_PATCH.read_text(), "tracked legacy patch", require_getifaddrs=False
    )
    print("libpeer Switch host candidate tests passed")


if __name__ == "__main__":
    main()
