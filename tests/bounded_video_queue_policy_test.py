#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HOST_CXX = os.environ.get("HOST_CXX", "c++")

with tempfile.TemporaryDirectory(prefix="lunarnx-bounded-policy-") as temp:
    binary = Path(temp) / "bounded_video_queue_policy_test"
    subprocess.run([
        HOST_CXX, "-std=c++17", "-pthread",
        str(ROOT / "tests/bounded_video_queue_policy_test.cpp"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)

print("Bounded video queue executable policy test passed")
