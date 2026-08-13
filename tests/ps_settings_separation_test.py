#!/usr/bin/env python3
from pathlib import Path


def main():
    activity = Path("src/ui/ps_activity.cpp").read_text()
    settings = Path("src/ui/ps_settings_activity.cpp").read_text()
    build = Path("Makefile.switch").read_text()

    assert "new PsSettingsActivity(loadPsSettings())" in activity
    assert "StreamSettingsActivity" not in activity
    assert "loadPsSettings()" in activity
    assert '"ps_resolution"' in settings
    assert '"ps_bitrate_kbps"' in settings
    assert "ButtonMappingProfile::PlayStation" in settings
    assert '"ps_video_codec"' in settings
    assert "HEVC (H.265)" in settings
    assert "force_region_ip" not in settings
    assert "xCloud" not in settings
    assert "ps_settings_activity.cpp" in build
    print("PS settings separation test passed")


if __name__ == "__main__":
    main()
