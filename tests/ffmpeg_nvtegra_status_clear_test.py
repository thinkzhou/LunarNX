#!/usr/bin/env python3
"""Regression checks for the Switch FFmpeg NVDEC status-buffer fix."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    patch_path = ROOT / "tools/ffmpeg_switch_build/nvtegra-status-clear.patch"
    host_script = ROOT / "tools/ffmpeg_switch_build/build.sh"
    container_script = ROOT / "tools/ffmpeg_switch_build/build_in_docker.sh"
    makefile = (ROOT / "Makefile.switch").read_text()

    require(patch_path.is_file(), "NVTEGRA status-clear patch must be tracked")
    patch = patch_path.read_text()
    hunk_pos = patch.find("@@ -248,0 +249,5 @@")
    memset_pos = patch.find("memset((uint8_t *)av_nvtegra_map_get_addr")
    require(hunk_pos >= 0,
            "patch must insert at the pinned pre-submit NVDEC source location")
    require(memset_pos >= 0, "patch must clear the mapped NVDEC status buffer")
    require("ctx->status_off" in patch, "status clear must start at status_off")
    require("ctx->cmdbuf_off - ctx->status_off" in patch,
            "status clear must stop before the command buffer")
    require(memset_pos > hunk_pos,
            "status must be cleared in the pre-submit insertion hunk")

    require(host_script.is_file(), "host Docker build wrapper must be tracked")
    host = host_script.read_text()
    require("devkitpro/devkita64:20251117" in host,
            "Switch FFmpeg build must pin the approved devkitA64 image")
    require("docker run" in host, "host build wrapper must compile through Docker")

    require(container_script.is_file(), "container FFmpeg build script must be tracked")
    container = container_script.read_text()
    require("e1094ac45d3bc7942043e72a23b6ab30faaddb8a" in container,
            "FFmpeg source revision must match wiliwili")
    require("88e5876bea9502d06f46a8656e3530684d3aaf7d" in container,
            "wiliwili patch revision must be pinned")
    require("nvtegra-status-clear.patch" in container,
            "container build must apply the local status-clear patch")
    require("BUILD_KEY_FILE" in container,
            "interrupted Docker builds must be resumable with a pinned build key")
    require("LOCAL_PATCH_SHA256" in container and
            "LOCAL_PATCH_SHA256" in container.split("BUILD_KEY=", 1)[1].splitlines()[0],
            "build key must change when the LunarNX NVTEGRA patch changes")
    require("FFMPEG_JOBS" in container and "FFMPEG_JOBS" in host,
            "Docker build parallelism must be configurable for Rosetta stability")

    require("FFMPEG_LIBDIR ?= lib/switch" in makefile,
            "Switch build must support an isolated FFmpeg library directory")
    require("FFMPEG_SWSCALE_LIB ?= -lswscale" in makefile,
            "Switch build must preserve the devkitPro swscale default")
    require("$(FFMPEG_LIBS)" in makefile,
            "LunarNX and Rho link lines must use the shared FFmpeg selection")

    print("FFmpeg NVTEGRA status-clear tests passed")


if __name__ == "__main__":
    main()
