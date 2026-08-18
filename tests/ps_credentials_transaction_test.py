#!/usr/bin/env python3

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="lunarnx-ps-credentials-") as temp:
        temp_path = Path(temp)
        binary = temp_path / "ps_credentials_transaction_test"
        cjson_object = temp_path / "cJSON.o"
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-Wno-deprecated-declarations",
                f"-I{ROOT / 'lib'}",
                "-c",
                str(ROOT / "lib/cJSON.c"),
                "-o",
                str(cjson_object),
            ],
            check=True,
        )
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-DLUNARNX_DESKTOP_TEST",
                "-Wno-deprecated-declarations",
                "-isystem",
                "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1",
                f"-I{ROOT / 'src'}",
                f"-I{ROOT / 'lib'}",
                str(ROOT / "tests/ps_credentials_transaction_test.cpp"),
                str(ROOT / "src/ps/ps_credentials.cpp"),
                str(cjson_object),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary), str(temp_path / "credentials.json")], check=True)

    print("PS credential transaction test passed")


if __name__ == "__main__":
    main()
