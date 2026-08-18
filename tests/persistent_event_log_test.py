#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
LIBCXX = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="lunarnx-event-log-") as temp:
        binary = Path(temp) / "persistent_event_log_test"
        subprocess.run([
            "clang++", "-std=c++17", "-DLUNARNX_DIAGNOSTIC_LOG=0",
            "-DLUNARNX_DROP_DIAGNOSTIC_LOG=0", "-isystem", LIBCXX,
            str(ROOT / "tests/persistent_event_log_test.cpp"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], cwd=temp, check=True)
    print("Persistent release event log test passed")


if __name__ == "__main__":
    main()
