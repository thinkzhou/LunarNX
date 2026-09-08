#!/usr/bin/env python3
"""Exercise physical-input release fencing and legacy routing with sanitizers."""
from pathlib import Path
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parents[1]
flags = []
if sys.platform == "darwin":
    sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
    flags = ["-isysroot", sdk, "-isystem", sdk + "/usr/include/c++/v1"]
with tempfile.TemporaryDirectory(prefix="lunar-input-router-") as directory:
    binary = str(Path(directory) / "test")
    subprocess.run(["clang++", "-std=c++17", "-fsanitize=address,undefined", *flags,
                    "-I" + str(root / "src"), str(root / "tests/stream_input_router_test.cpp"),
                    "-o", binary], check=True)
    subprocess.run([binary], check=True)
print("PASS: held buttons, triggers, sticks, repeated UI transitions and legacy input routing")
