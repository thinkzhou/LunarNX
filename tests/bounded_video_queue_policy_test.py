#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LIBCXX = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"

with tempfile.TemporaryDirectory(prefix="lunarnx-bounded-policy-") as temp:
    binary = Path(temp) / "bounded_video_queue_policy_test"
    subprocess.run([
        "clang++", "-std=c++17", "-pthread", "-isystem", LIBCXX,
        str(ROOT / "tests/bounded_video_queue_policy_test.cpp"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)

print("Bounded video queue executable policy test passed")
