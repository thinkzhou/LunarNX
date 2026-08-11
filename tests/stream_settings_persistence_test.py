#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    settings = Path("src/ui/stream_settings_activity.cpp").read_text()
    auth = Path("src/ui/auth_activity.cpp").read_text()
    main = Path("src/ui/main_activity.cpp").read_text()

    for key in ("resolution", "bitrate", "video_backend",
                "post_process_mode", "dithering"):
        require(f'"{key}"' in settings,
                f"stream settings must persist {key}")
    require("saveStreamSettings(settings_)" in settings,
            "closing settings must persist the complete stream profile")
    require("loadStreamSettings" in auth,
            "controller creation must load persisted stream settings")
    require("loadStreamSettings" in main,
            "Xbox pages must initialize their snapshot from persisted settings")

    print("Stream settings persistence tests passed")


if __name__ == "__main__":
    main()
