#!/usr/bin/env python3
"""Compatibility entry point for the single tracked Chiaki Switch patch."""

import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} CHIAKI_SOURCE", file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).resolve()
    patch = Path(__file__).resolve().parent / "chiaki_switch" / "lunarnx-chiaki-switch.patch"
    check = subprocess.run(
        ["git", "-C", str(source), "apply", "--check", str(patch)],
        stdout=subprocess.DEVNULL,
    )
    if check.returncode == 0:
        subprocess.run(["git", "-C", str(source), "apply", str(patch)], check=True)
        return 0
    reverse = subprocess.run(
        ["git", "-C", str(source), "apply", "--reverse", "--check", str(patch)],
        stdout=subprocess.DEVNULL,
    )
    if reverse.returncode == 0:
        return 0
    print("Chiaki checkout does not match the tracked LunarNX patch", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
