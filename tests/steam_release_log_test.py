#!/usr/bin/env python3
"""Verify essential Steam evidence survives APP_DIAG=0 using real log files."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
flags = []
if sys.platform == "darwin":
    sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
    flags = ["-isysroot", sdk, "-isystem", sdk + "/usr/include/c++/v1"]
with tempfile.TemporaryDirectory(prefix="steam-release-log-") as directory:
    folder = Path(directory)
    binary = folder / "test"
    for asynchronous in (0, 1):
        run_folder = folder / str(asynchronous)
        run_folder.mkdir()
        subprocess.run(["clang++", *flags, "-std=c++20", "-pthread",
                        "-DLUNARNX_DIAGNOSTIC_LOG=0",
                        f"-DLUNARNX_DROP_DIAGNOSTIC_LOG={asynchronous}", "-Isrc",
                        "tests/steam_release_log_test.cpp", "-o", str(binary)],
                       cwd=root, check=True)
        subprocess.run([str(binary)], cwd=run_folder, check=True)
        contents = (run_folder / "lunarnx.log").read_text()
        assert "video_rx=42 hid_open=1" in contents
        assert "SteamNegotiation host config confirmed" in contents
        assert "verbose-only" not in contents
print("PASS: sync/async persistent Steam events remain in APP_DIAG=0; verbose logs stay disabled")
