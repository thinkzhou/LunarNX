#!/usr/bin/env python3
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def require(text: str, fragment: str, source: str) -> None:
    assert fragment in text, f"{source} must contain {fragment!r}"


version = (ROOT / "version.txt").read_text().strip()
assert re.fullmatch(
    r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)",
    version,
), f"version.txt must contain a release SemVer, got: {version}"

manifest = json.loads((ROOT / ".release-please-manifest.json").read_text())
assert manifest == {".": version}

config = json.loads((ROOT / "release-please-config.json").read_text())
root_package = config["packages"]["."]
assert root_package["release-type"] == "simple"
assert root_package["include-component-in-tag"] is False
assert root_package["package-name"] == "LunarNX"

cmake = (ROOT / "CMakeLists.txt").read_text()
require(cmake, 'file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt"', "CMake")
require(cmake, "project(LunarNX VERSION ${LUNARNX_PROJECT_VERSION}", "CMake")
assert "project(LunarNX VERSION 0." not in cmake

release_workflow = (ROOT / ".github/workflows/release-please.yml").read_text()
require(
    release_workflow,
    "googleapis/release-please-action@v5",
    "release workflow",
)
require(release_workflow, "release_created:", "release workflow")
require(release_workflow, "tag_name:", "release workflow")
require(
    release_workflow,
    "uses: ./.github/workflows/build-switch.yml",
    "release workflow",
)
require(
    release_workflow,
    "release_tag: ${{ needs.release-please.outputs.tag_name }}",
    "release workflow",
)

build_workflow = (ROOT / ".github/workflows/build-switch.yml").read_text()
require(build_workflow, "workflow_call:", "build workflow")
require(build_workflow, "actions/download-artifact@v8", "build workflow")
require(build_workflow, "permissions:\n      contents: write", "build workflow")
require(build_workflow, "gh release create", "build workflow")
require(build_workflow, "gh release upload", "build workflow")
require(build_workflow, "--clobber", "build workflow")

resolver = ROOT / "scripts/resolve_app_version.sh"
base_env = os.environ | {"GIT_COMMIT_SHA": "abcdef0123456789"}

development = subprocess.run(
    ["bash", str(resolver)],
    cwd=ROOT,
    env=base_env,
    check=True,
    capture_output=True,
    text=True,
)
assert development.stdout.strip() == f"{version}-gabcdef0"

release = subprocess.run(
    ["bash", str(resolver)],
    cwd=ROOT,
    env=base_env | {"RELEASE_TAG": f"v{version}"},
    check=True,
    capture_output=True,
    text=True,
)
assert release.stdout.strip() == version

major, minor, patch = (int(part) for part in version.split("."))
mismatched_version = f"{major}.{minor}.{patch + 1}"

mismatched_release = subprocess.run(
    ["bash", str(resolver)],
    cwd=ROOT,
    env=base_env | {"RELEASE_TAG": f"v{mismatched_version}"},
    capture_output=True,
    text=True,
)
assert mismatched_release.returncode == 2
assert "does not match version.txt" in mismatched_release.stderr

invalid_tag = subprocess.run(
    ["bash", str(resolver)],
    cwd=ROOT,
    env=base_env | {"RELEASE_TAG": "release-1.4.2"},
    capture_output=True,
    text=True,
)
assert invalid_tag.returncode == 2
assert "vMAJOR.MINOR.PATCH" in invalid_tag.stderr

with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8") as invalid_version:
    invalid_version.write("1.4\n")
    invalid_version.flush()
    bad_version = subprocess.run(
        ["bash", str(resolver)],
        cwd=ROOT,
        env=base_env | {"VERSION_FILE": invalid_version.name},
        capture_output=True,
        text=True,
    )
assert bad_version.returncode == 2
assert "version.txt must contain" in bad_version.stderr

print("GitHub release workflow regression checks passed")
