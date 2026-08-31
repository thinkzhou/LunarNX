#!/usr/bin/env python3
import subprocess
import tempfile
from pathlib import Path

LIBCXX = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="lunarnx-ps-nat-") as temp:
        binary = Path(temp) / "ps_remote_retry_policy_test"
        subprocess.run([
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-isystem", LIBCXX,
            "-I", str(root / "src"),
            str(root / "tests/ps_remote_retry_policy_test.cpp"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    print("PS remote retry policy tests passed")


if __name__ == "__main__":
    main()
