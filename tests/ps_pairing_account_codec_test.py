#!/usr/bin/env python3
from pathlib import Path
import os
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
LIBCXX = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1")


def compiler() -> str:
    configured = os.environ.get("CXX")
    if configured:
        return configured
    for candidate in ("c++", "g++", "clang++"):
        path = shutil.which(candidate)
        if path:
            return path
    raise RuntimeError("no C++ compiler found (tried CXX, c++, g++, clang++)")


def main() -> None:
    with tempfile.TemporaryDirectory() as temp:
        binary = Path(temp) / "ps_pairing_account_codec_test"
        command = [
            compiler(), "-std=c++17", "-isystem", LIBCXX,
            "-I", str(ROOT / "src"),
            str(ROOT / "tests/ps_pairing_account_codec_test.cpp"),
            str(ROOT / "src/ps/ps_pairing_account_codec.cpp"),
            str(ROOT / "src/ps/psn_auth_utils.cpp"),
            "-o", str(binary),
        ]
        if not LIBCXX.exists():
            command.remove("-isystem")
            command.remove(LIBCXX)
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    print("PS pairing Account ID codec tests passed")


if __name__ == "__main__":
    main()
