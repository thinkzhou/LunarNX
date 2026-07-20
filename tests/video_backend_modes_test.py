#!/usr/bin/env python3
import re
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    media = Path("src/stream/media_pipeline.h").read_text()
    controller = Path("src/app/stream_controller.h").read_text()
    auth = Path("src/ui/auth_activity.cpp").read_text()
    main_header = Path("src/ui/main_activity.h").read_text()
    settings = Path("src/ui/stream_settings_activity.cpp").read_text()
    config = Path("config/default_config.json").read_text()

    require(
        re.search(
            r"enum class VideoBackend\s*\{\s*"
            r"HardwareZeroCopy,\s*HardwareCopyOut,\s*Software,\s*\};",
            media,
        ),
        "VideoBackend must expose zero-copy, copy-out, and software modes",
    )
    for backend, name in (
        ("HardwareZeroCopy", "hardware_zero_copy"),
        ("HardwareCopyOut", "hardware_copy_out"),
        ("Software", "software"),
    ):
        require(
            f'case VideoBackend::{backend}: return "{name}";' in media,
            f"VideoBackend::{backend} must use the stable name {name}",
        )

    require(
        "VideoBackend video_backend = VideoBackend::HardwareZeroCopy" in media,
        "Full hardware must be the pipeline default",
    )
    require(
        "default_video_backend_{stream::VideoBackend::HardwareZeroCopy}" in controller,
        "Full hardware must be the controller default",
    )
    require(
        "video_backend_ = stream::VideoBackend::HardwareZeroCopy" in main_header,
        "Full hardware must be the UI default",
    )

    for value, backend in (
        ("hardware_zero_copy", "HardwareZeroCopy"),
        ("hardware_copy_out", "HardwareCopyOut"),
        ("software", "Software"),
        ("hardware", "HardwareCopyOut"),
    ):
        require(
            re.search(
                rf'strcmp\(value, "{value}"\) == 0\)\s*\{{\s*'
                rf"return stream::VideoBackend::{backend};",
                auth,
            ),
            f"Config value {value} must select VideoBackend::{backend}",
        )

    require(
        '"video_backend": "hardware_zero_copy"' in config,
        "Default config must select full hardware",
    )
    for key in ("decoder", "decoder_hardware", "decoder_copy", "decoder_software"):
        require(
            f'"lunarnx/settings/{key}"' in settings,
            f"Dedicated settings must localize {key}",
        )
    for selected, backend in (
        ("1", "HardwareCopyOut"),
        ("2", "Software"),
    ):
        require(
            re.search(
                rf"selected == {selected}\)\s*\{{\s*"
                rf"settings_\.video_backend = stream::VideoBackend::{backend};",
                settings,
            ),
            f"Settings selector must map index {selected} to {backend}",
        )
    require(
        "settings_.video_backend = stream::VideoBackend::HardwareZeroCopy;" in settings,
        "Settings selector must map the default index to full hardware",
    )

    print("Video backend mode tests passed")


if __name__ == "__main__":
    main()
