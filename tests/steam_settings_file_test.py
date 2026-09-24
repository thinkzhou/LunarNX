#!/usr/bin/env python3
"""Real temporary files with injected Switch-style no-overwrite rename behavior."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
flags = []
if sys.platform == "darwin":
    sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
    flags = ["-isysroot", sdk, "-isystem", sdk + "/usr/include/c++/v1"]
with tempfile.TemporaryDirectory(prefix="steam-settings-test-") as directory:
    exe = str(Path(directory) / "test")
    subprocess.run(["clang++", *flags, "-std=c++17", "-fsanitize=address,undefined",
                    "-g", "-Isrc", "tests/steam_settings_file_test.cpp", "-o", exe],
                   cwd=root, check=True)
    subprocess.run([exe, directory], check=True)
