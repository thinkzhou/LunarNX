#!/usr/bin/env python3
"""Exercise Chiaki's real LAN registration sockets against a local fake host."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    result = subprocess.run(
        [str(ROOT / "tools/chiaki_registration_probe/run_macos.sh")],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
        timeout=20,
    )
    output = result.stdout
    assert "PS4 search=SRC2 request=/sie/ps4/rp/sess/rgst result=PASS" in output
    assert "PS5 search=SRC3 request=/sie/ps5/rp/sess/rgst result=PASS" in output
    print("PS registration handshake test passed")


if __name__ == "__main__":
    main()
