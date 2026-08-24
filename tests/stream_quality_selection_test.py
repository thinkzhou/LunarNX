#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def read(path: str) -> str:
    file_path = ROOT / path
    require(file_path.exists(), f"missing {path}")
    return file_path.read_text()


def main() -> None:
    profile = read("src/app/stream_profile.h")
    controller_header = read("src/app/stream_controller.h")
    controller_source = read("src/app/stream_controller.cpp")
    settings_header = read("src/ui/stream_settings_activity.h")
    settings_source = read("src/ui/stream_settings_activity.cpp")
    loading_header = read("src/ui/stream_loading_activity.h")
    loading_source = read("src/ui/stream_loading_activity.cpp")
    main_source = read("src/ui/main_activity.cpp")

    require('"lunarnx/settings/resolution_1080p_hq"' in settings_source,
            "settings must expose a distinct 1080p HQ option")
    require("settings_.bitrate_kbps >= 30000 ? 2 : 1" in settings_source,
            "settings must restore the HQ selection")
    for bitrate in ("10000", "20000", "30000"):
        require(bitrate in settings_source,
                f"settings must map a quality tier to {bitrate} kbps")

    require("bitrate_kbps" in settings_header and
            "bitrate_kbps" in loading_header,
            "quality bitrate must cross the UI request snapshots")
    require("request.bitrate_kbps = stream_bitrate_kbps_" in main_source and
            "stream_bitrate_kbps_ = updated.bitrate_kbps" in main_source,
            "MainActivity must preserve the selected bitrate for both targets")
    require("request.bitrate_kbps)" in loading_source,
            "loading activity must forward bitrate to the controller")
    require("const stream::MediaPipelineOptions& options,\n                     int bitrate_kbps" in controller_header,
            "controller must expose a bitrate-aware launch path")
    require("makeCloudStreamProfile(\n        title_id, width, height, bitrate_kbps)" in controller_source,
            "cloud launches must build the selected quality profile")
    require('profile.os_name = height >= 1080 ? "windows" : "android"' in profile,
            "cloud 1080p HQ must not implicitly request the Tizen 1440p tier")

    print("Stream quality selection tests passed")


if __name__ == "__main__":
    main()
