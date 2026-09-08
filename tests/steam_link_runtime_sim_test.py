#!/usr/bin/env python3
"""Run production portable streaming helpers under ASan/UBSan and real H264 decode."""
from pathlib import Path
import re
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        print(result.stderr, file=sys.stderr)
        result.check_returncode()
    return result.stdout


with tempfile.TemporaryDirectory(prefix="lunarnx-steam-sim-") as directory:
    tmp = Path(directory)
    binary = str(tmp / "sim")
    sdk_flags = []
    if sys.platform == "darwin":
        sdk = run("xcrun", "--show-sdk-path").strip()
        sdk_flags = ["-isysroot", sdk, "-isystem", str(Path(sdk) / "usr/include/c++/v1")]
    run("clang++", *sdk_flags, "-std=c++20", "-pthread", "-g", "-fsanitize=address,undefined",
        "-Isrc", "tests/steam_link_runtime_sim.cpp", "-o", binary)
    source = str(tmp / "source.h264")
    run("ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=30",
        "-frames:v", "5", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-f", "h264", source)
    data = Path(source).read_bytes()
    starts = list(re.finditer(b"\x00\x00(?:\x00)?\x01", data))
    config, frames = bytearray(), bytearray()
    for i, match in enumerate(starts):
        end = starts[i + 1].start() if i + 1 < len(starts) else len(data)
        nal = data[match.end():end]
        (config if nal[0] & 31 in (7, 8) else frames).extend(b"\x00\x00\x00\x01" + nal)
    (tmp / "config").write_bytes(config)
    (tmp / "frames").write_bytes(frames)
    output = str(tmp / "reconstructed.h264")
    print(run(binary, str(tmp / "config"), str(tmp / "frames"), output).strip())
    original = run("ffmpeg", "-v", "error", "-i", source, "-f", "framemd5", "-")
    decoded = run("ffmpeg", "-v", "error", "-i", output, "-f", "framemd5", "-")
    assert original == decoded, "Reconstructed frames differ from the original decode"
    print("PASS: separated SPS/PPS + 5 video frames reconstruct and decode identically")
