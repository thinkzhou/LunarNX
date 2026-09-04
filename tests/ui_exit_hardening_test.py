#!/usr/bin/env python3
"""Source contracts for Borealis/Deko3D exit hardening."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SETUP = (ROOT / "scripts/setup_dependencies.sh").read_text()
BUILD = (ROOT / "scripts/build_switch_in_container.sh").read_text()
PATCH_PATH = ROOT / "tools/borealis_switch/lunarnx-borealis-command-buffer.patch"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require(PATCH_PATH.exists(), "Borealis command-buffer hardening patch must be tracked")
patch = PATCH_PATH.read_text()
require(
    "DynamicCmdSize = 0x20000" in patch and
    "DynamicCmdSize = 0x40000" in patch,
    "Borealis patch must increase dynamic Deko3D command memory to 256 KiB",
)
for script_name, script in (("setup", SETUP), ("build", BUILD)):
    require(
        "lunarnx-borealis-command-buffer.patch" in script,
        f"{script_name} script must apply or validate the command-buffer patch",
    )

print("UI exit hardening test passed")
