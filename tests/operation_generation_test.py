#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
LIBCXX = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"


with tempfile.TemporaryDirectory() as temp:
    binary = Path(temp) / "operation_generation_test"
    subprocess.run(
        [
            "clang++",
            "-std=c++17",
            "-pthread",
            "-isystem",
            LIBCXX,
            f"-I{ROOT / 'src'}",
            str(ROOT / "tests/operation_generation_test.cpp"),
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        check=True,
    )
    subprocess.run([str(binary)], check=True)

print("operation generation tests passed")
