#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(text: str, fragment: str, source: str) -> None:
    assert fragment in text, f"{source} must contain {fragment!r}"


workflow = (ROOT / ".github/workflows/build-switch.yml").read_text()
require(workflow, "devkitpro/devkita64:20251117", "workflow")
require(workflow, "./scripts/setup_dependencies.sh", "workflow")
require(workflow, "./scripts/setup_chiaki_dependencies.sh", "workflow")
require(workflow, "./tools/chiaki_switch/build_in_container.sh", "workflow")
require(workflow, "./scripts/build_switch_in_container.sh", "workflow")
require(workflow, "CURL_PROVIDER: moonlight", "workflow")
require(workflow, "tests/switch_nro_bss_test.py", "workflow")
require(workflow, "actions/checkout@v5", "workflow")
require(workflow, 'safe.directory "$GITHUB_WORKSPACE"', "workflow")
require(workflow, "actions/upload-artifact@v7", "workflow")
assert "actions/create-release" not in workflow
assert "softprops/action-gh-release" not in workflow

switch_wrapper = (ROOT / "scripts/docker_build_full.sh").read_text()
require(switch_wrapper, "build_switch_in_container.sh", "Switch Docker wrapper")
require(switch_wrapper, 'APP_VERSION="${APP_VERSION:-0.2.0}"', "Switch Docker wrapper")
require(switch_wrapper, "DEV_BRIDGE_UPLOAD_TOKEN", "Switch Docker wrapper")

chiaki_wrapper = (ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()
require(chiaki_wrapper, "build_in_container.sh", "Chiaki Docker wrapper")

chiaki_setup = (ROOT / "scripts/setup_chiaki_dependencies.sh").read_text()
require(chiaki_setup, "https://github.com/xlanor/chiaki-ng.git", "Chiaki setup")
require(chiaki_setup, "1597a48514e5d9e67168ca40e6fa40c0171cd379", "Chiaki setup")

dependency_setup = (ROOT / "scripts/setup_dependencies.sh").read_text()
for nested_patch in (
    "0001-switch-add-libnx-network-byte-order-includes.patch",
    "0001-switch-configure-mbedtls-for-DTLS-SRTP.patch",
    "0001-switch-add-libnx-compatibility-stubs.patch",
):
    require(dependency_setup, nested_patch, "dependency setup")

libpeer_patch = (
    ROOT / "tools/libpeer_legacy/0001-switch-adapt-libpeer-WebRTC-path.patch"
).read_text()
for unavailable_commit in (
    "d95116d1ee69cf6439c6523fd304bec9d9b7f5a6",
    "f815c0d349edf83c3f86f3e864e85da852e49555",
    "a8db67a8b25ac391f8b93747ac8b712d64bf5a01",
):
    assert unavailable_commit not in libpeer_patch

chiaki_build = (ROOT / "tools/chiaki_switch/build_in_container.sh").read_text()
require(chiaki_build, "tools/chiaki_switch/pbgen", "Chiaki container build")
require(chiaki_build, "lunarnx-chiaki-packetstats-wrap.patch", "Chiaki container build")
require(chiaki_build, "lunarnx-chiaki-key-position-diagnostics.patch", "Chiaki container build")
assert "/work/github_repos/chiaki-ng/pbgen" not in chiaki_build

protoc_wrapper = (ROOT / "tools/chiaki_switch/protoc_from_pbgen.sh").read_text()
require(protoc_wrapper, "LUNARNX_CHIAKI_PBGEN", "Chiaki protoc wrapper")
assert "/work/github_repos/chiaki-ng/pbgen" not in protoc_wrapper

print("GitHub Actions Switch build regression checks passed")
