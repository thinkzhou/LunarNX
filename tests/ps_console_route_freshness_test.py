#!/usr/bin/env python3

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
LIBCXX = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="lunarnx-ps-route-") as temp:
        binary = Path(temp) / "ps_console_route_freshness_test"
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-DLUNARNX_DESKTOP_TEST",
                "-isystem",
                LIBCXX,
                f"-I{ROOT / 'src'}",
                str(ROOT / "tests/ps_console_route_freshness_test.cpp"),
                str(ROOT / "src/ps/ps_console_resolver.cpp"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)

    print("PS console route freshness test passed")


if __name__ == "__main__":
    main()
