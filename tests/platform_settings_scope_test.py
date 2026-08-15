#!/usr/bin/env python3
from pathlib import Path


def main():
    source = Path("src/ui/stream_settings_activity.cpp").read_text()

    assert "if (scope_ == StreamSettingsScope::Global)" in source
    assert "if (scope_ == StreamSettingsScope::Xbox)" in source
    assert "addGlobalSettings" in source
    assert "addXboxSettings" in source
    print("platform settings scope tests passed")


if __name__ == "__main__":
    main()
