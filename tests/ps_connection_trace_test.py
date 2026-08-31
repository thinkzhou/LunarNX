#!/usr/bin/env python3
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LIBCXX = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"


with tempfile.TemporaryDirectory(prefix="lunarnx-ps-trace-") as temp:
    temp_path = Path(temp)
    binary = temp_path / "ps_connection_trace_test"
    subprocess.run(
        [
            "clang++",
            "-std=c++20",
            "-pthread",
            "-D__SWITCH__",
            "-DLUNARNX_DIAGNOSTIC_LOG=0",
            "-DLUNARNX_DROP_DIAGNOSTIC_LOG=0",
            "-isystem",
            LIBCXX,
            "-I",
            str(ROOT),
            str(ROOT / "tests/ps_connection_trace_test.cpp"),
            str(ROOT / "src/ps/ps_connection_trace.cpp"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], cwd=temp_path, check=True)

print("PS connection trace runtime test passed")
