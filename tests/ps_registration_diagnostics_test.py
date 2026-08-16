#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
LIBCXX = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="lunarnx-ps-reg-diag-") as temp:
        binary = Path(temp) / "ps_registration_diagnostics_test"
        subprocess.run([
            "clang++", "-std=c++17", "-isystem", LIBCXX,
            str(ROOT / "tests/ps_registration_diagnostics_test.cpp"),
            str(ROOT / "src/ps/ps_registration_diagnostics.cpp"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    print("PS registration diagnostics test passed")


if __name__ == "__main__":
    main()
