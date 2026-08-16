#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
LIBCXX = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="lunarnx-ps-account-store-") as temp:
        temp_path = Path(temp)
        cjson_object = temp_path / "cJSON.o"
        binary = temp_path / "store_test"
        config = temp_path / "config.json"
        subprocess.run([
            "clang", "-std=c11", "-I", str(ROOT / "lib"), "-c",
            str(ROOT / "lib/cJSON.c"), "-o", str(cjson_object),
        ], check=True)
        subprocess.run([
            "clang++", "-std=c++17", "-isystem", LIBCXX,
            "-I", str(ROOT / "src"), "-I", str(ROOT / "lib"),
            str(ROOT / "tests/ps_pairing_account_store_test.cpp"),
            str(ROOT / "src/ps/ps_pairing_account_store.cpp"),
            str(cjson_object), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary), str(config)], check=True)
    print("PS per-console pairing Account ID store tests passed")


if __name__ == "__main__":
    main()
