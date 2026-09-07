#!/usr/bin/env python3
"""Replay a host's HID requests through the production provider and real ihslib."""
from pathlib import Path
import concurrent.futures
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
def run(args):
    r = subprocess.run([str(a) for a in args], cwd=ROOT, capture_output=True, text=True, timeout=60)
    if r.returncode:
        print(r.stdout + r.stderr, file=sys.stderr)
        r.check_returncode()
    return r.stdout

ihs = ROOT / "vendor/ihslib/src"
mbed = ROOT / "lib/libpeer/third_party/mbedtls"
includes = ["-Isrc", "-Ivendor/ihslib/include", "-Ivendor/ihslib/src", "-Ivendor/protobuf-c", f"-I{mbed / 'include'}"]
san = ["-fsanitize=address,undefined", "-g", "-O1"]
sdk = run(["xcrun", "--show-sdk-path"]).strip() if sys.platform == "darwin" else ""
cflags = ["-isysroot", sdk] if sdk else []
cppflags = cflags + (["-isystem", f"{sdk}/usr/include/c++/v1"] if sdk else [])
sources = list(ihs.glob("*.c"))
for sub in ("client", "session", "protobuf"):
    sources += list((ihs / sub).rglob("*.c"))
sources += list((ihs / "hid").glob("*.c"))
overrides = {ihs / "hid/device.c": ROOT / "src/steamlink/ihs_hid_device.c",
             ihs / "ihs_timer.c": ROOT / "src/steamlink/ihs_timer.c",
             ihs / "client/streaming.c": ROOT / "src/steamlink/ihs_streaming.c",
             ihs / "hid/manager.c": ROOT / "src/steamlink/ihs_hid_manager.c",
             ihs / "session/session.c": ROOT / "src/steamlink/ihs_session.c"}
sources = [overrides.get(source, source) for source in sources]
sources += [ihs / "platforms/ihs_ip_posix.c", ihs / "platforms/ihs_udp_posix.c", ihs / "crypto/impl_mbedtls.c",
            ROOT / "vendor/protobuf-c/protobuf-c/protobuf-c.c", ROOT / "tools/steamlink_probe/posix_thread.c"]
with tempfile.TemporaryDirectory(prefix="lunarnx-hid-sim-") as directory:
    tmp = Path(directory)
    def compile_item(item):
        i, source = item
        obj = tmp / f"{i}.o"
        run(["clang", *cflags, *san, *includes, "-std=c11", "-D_DARWIN_C_SOURCE", "-include", "math.h", "-c", source, "-o", obj])
        return obj
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
        objects = list(executor.map(compile_item, enumerate(sources)))
    exe = tmp / "sim"
    run(["clang++", *cppflags, *san, *includes, "-std=c++20", "-pthread",
         "src/steamlink/steam_hid.cpp", "tests/steam_link_hid_sim.cpp", *objects,
         f"-L{mbed / 'build_host/library'}", "-lmbedcrypto", "-lmbedtls", "-lmbedx509", "-o", exe])
    print(run([exe]).strip())
