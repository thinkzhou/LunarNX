#!/usr/bin/env python3
import os
import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVKITPRO = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))
DOCKER_IMAGE = os.environ.get(
    "LUNARNX_DEVKIT_IMAGE", "devkitpro/devkita64:20251117"
)
CC = DEVKITPRO / "devkitA64/bin/aarch64-none-elf-gcc"
OBJDUMP = DEVKITPRO / "devkitA64/bin/aarch64-none-elf-objdump"
ARCHIVE = ROOT / "lib/switch/libchiaki.a"

FIELDS = {
    "session_size": "chiaki_session_size",
    "session_log_offset": "chiaki_session_log_offset",
    "session_holepunch_offset": "chiaki_session_holepunch_offset",
    "stream_connection_size": "chiaki_stream_connection_size",
    "controller_state_offset": "chiaki_controller_state_offset",
}


def fail(message):
    raise SystemExit(f"FAIL: {message}")


def disassembly(path):
    return subprocess.check_output(
        [str(OBJDUMP), "-d", str(path)], text=True, stderr=subprocess.STDOUT
    )


def returned_constants(text, prefix):
    values = {}
    current = None
    symbol_re = re.compile(rf"^[0-9a-f]+ <{re.escape(prefix)}([^>]+)>:$")
    move_re = re.compile(r"\bmov\s+x0,\s+#0x([0-9a-f]+)")
    for raw_line in text.splitlines():
        line = raw_line.strip()
        symbol = symbol_re.match(line)
        if symbol:
            current = symbol.group(1)
            continue
        if current:
            move = move_re.search(line)
            if move:
                values[current] = int(move.group(1), 16)
                current = None
    return values


def main():
    makefile = (ROOT / "Makefile.switch").read_text()
    if "chiaki-abi-check" not in makefile:
        fail("Switch link does not depend on the Chiaki ABI check")

    if os.environ.get("LUNARNX_CHIAKI_ABI_IN_CONTAINER") != "1":
        subprocess.run(
            [
                "docker", "run", "--rm", "--platform", "linux/amd64",
                "-e", "LUNARNX_CHIAKI_ABI_IN_CONTAINER=1",
                "-e", "DEVKITPRO=/opt/devkitpro",
                "-v", f"{ROOT}:/work", "-w", "/work",
                DOCKER_IMAGE, "python3", "tests/chiaki_switch_abi_test.py",
            ],
            check=True,
        )
        return

    for tool in (CC, OBJDUMP):
        if not tool.exists():
            fail(f"required devkitA64 tool is missing: {tool}")
    if not ARCHIVE.exists():
        fail(f"Chiaki archive is missing: {ARCHIVE}")

    with tempfile.TemporaryDirectory(prefix="lunarnx-chiaki-abi-") as temp:
        consumer_object = Path(temp) / "abi_probe.o"
        subprocess.run(
            [
                str(CC),
                "-D__SWITCH__",
                "-DCHIAKI_LIB_ENABLE_LIBNX_CRYPTO",
                f"-I{ROOT / 'lib/switch/include'}",
                f"-I{ROOT / 'lib/libpeer/third_party/mbedtls/include'}",
                f"-I{DEVKITPRO / 'libnx/include'}",
                f"-I{DEVKITPRO / 'portlibs/switch/include'}",
                "-c",
                str(ROOT / "tools/chiaki_switch/abi_probe.c"),
                "-o",
                str(consumer_object),
            ],
            check=True,
        )

        consumer = returned_constants(
            disassembly(consumer_object), "lunarnx_consumer_chiaki_"
        )
        library = returned_constants(
            disassembly(ARCHIVE), "lunarnx_library_chiaki_"
        )

    missing_consumer = sorted(set(FIELDS) - set(consumer))
    if missing_consumer:
        fail(f"consumer ABI probe is incomplete: {', '.join(missing_consumer)}")
    missing_library = sorted(set(FIELDS) - set(library))
    if missing_library:
        fail(
            "libchiaki.a has no complete ABI fingerprint; rebuild it with "
            f"tools/chiaki_switch/build_in_docker.sh (missing: {', '.join(missing_library)})"
        )

    mismatches = [
        f"{field}: consumer={consumer[field]} library={library[field]}"
        for field in FIELDS
        if consumer[field] != library[field]
    ]
    if mismatches:
        fail("Chiaki ABI mismatch: " + "; ".join(mismatches))

    print("Chiaki Switch ABI test passed")


if __name__ == "__main__":
    main()
