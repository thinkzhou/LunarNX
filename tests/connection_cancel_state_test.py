#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
LIBCXX = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"


with tempfile.TemporaryDirectory() as temp:
    binary = Path(temp) / "connection_cancel_state_test"
    command = [
        os.environ.get("CXX", "c++"),
        "-std=c++17",
        "-pthread",
    ]
    if Path(LIBCXX).exists():
        command.extend(["-isystem", LIBCXX])
    command.extend([
        f"-I{ROOT / 'src'}",
        str(ROOT / "tests/connection_cancel_state_test.cpp"),
        "-o",
        str(binary),
    ])
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True)

print("connection cancel state tests passed")
