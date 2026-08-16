#!/usr/bin/env python3
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "lib/switch/libchiaki.a"
FORBIDDEN = (b"LUNARNX-PSRX", b"LUNARNX-PSVIDEO", b"LUNARNX-PSKEY")


def main() -> None:
    if not ARCHIVE.is_file():
        raise AssertionError("Build the default Chiaki Switch SDK before this test")

    strings = subprocess.run(
        ["strings", str(ARCHIVE)], check=True, capture_output=True
    ).stdout
    present = [marker.decode() for marker in FORBIDDEN if marker in strings]
    if present:
        raise AssertionError(
            "Release Chiaki archive still contains transport diagnostics: "
            + ", ".join(present)
        )

    print("Chiaki release transport diagnostics test passed")


if __name__ == "__main__":
    main()
