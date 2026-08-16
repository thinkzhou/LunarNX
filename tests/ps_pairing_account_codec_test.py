#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
LIBCXX = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"


def main() -> None:
    with tempfile.TemporaryDirectory() as temp:
        binary = Path(temp) / "ps_pairing_account_codec_test"
        subprocess.run([
            "clang++", "-std=c++17", "-isystem", LIBCXX,
            "-I", str(ROOT / "src"),
            str(ROOT / "tests/ps_pairing_account_codec_test.cpp"),
            str(ROOT / "src/ps/ps_pairing_account_codec.cpp"),
            str(ROOT / "src/ps/psn_auth_utils.cpp"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    print("PS pairing Account ID codec tests passed")


if __name__ == "__main__":
    main()
