#!/usr/bin/env python3
"""Network-backed clean-checkout test of pinned Steam dependency preparation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


with tempfile.TemporaryDirectory(prefix="lunarnx-steam-deps-") as temporary:
    checkout = Path(temporary) / "checkout"
    run("git", "clone", "--quiet", "--shared", "--no-recurse-submodules", str(root), str(checkout))
    headers = {"vendor/ihslib": "include/ihslib/client.h",
               "vendor/protobuf-c": "protobuf-c/protobuf-c.h"}
    for path, header in headers.items():
        assert not (checkout / path / header).exists(), "Must start without submodules"
    for _ in range(2):
        run("bash", str(root / "scripts/setup_steamlink_dependencies.sh"), str(checkout))
        for path, header in headers.items():
            assert (checkout / path / header).is_file(), header
            expected = subprocess.check_output(
                ["git", "-C", str(checkout), "rev-parse", f"HEAD:{path}"], text=True).strip()
            actual = subprocess.check_output(
                ["git", "-C", str(checkout / path), "rev-parse", "HEAD"], text=True).strip()
            assert actual == expected, (path, expected, actual)
print("PASS: clean checkout downloads pinned Steam headers; repeated setup preserves revisions")
