#!/usr/bin/env python3
"""Static checks for the Switch Steam Link integration boundary."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> None:
    makefile = read("Makefile.switch")
    client = read("src/steamlink/steam_link_client.cpp")
    activity = read("src/ui/steam_link_activity.cpp")
    platform = read("src/ui/platform_activity.cpp")
    desktop_probe = read("tools/steamlink_probe/desktop_stream_probe.c")
    desktop_build = read("tools/steamlink_probe/build_desktop_stream_probe.sh")

    assert "STEAMLINK ?= 1" in makefile
    assert "vendor/ihslib/src/client/discovery.c" in makefile
    assert "vendor/ihslib/src/client/authorization.c" in makefile
    assert "vendor/protobuf-c/protobuf-c/protobuf-c.c" in makefile
    assert "IHS_ClientStartDiscovery(client_, 2500)" in client
    assert "IHS_ClientAuthorizationRequest(client_, &protocol_host, pin.c_str())" in client
    assert "pin.size() != 4" in client
    assert "UDP" not in activity  # UI does not own the protocol transport.
    assert "new SteamLinkActivity()" in platform
    assert "steamlink_device.json" in read(".gitignore")
    assert "IHS_ClientStreamingRequest" in desktop_probe
    assert "IHS_SessionConnect" in desktop_probe
    assert "video start codec" in desktop_probe
    assert "audio start codec" in desktop_probe
    assert "--pin SECURITY_PIN" in desktop_probe
    assert "session/channels/video/ch_data_video.c" in desktop_build

    print("steam link integration checks passed")


if __name__ == "__main__":
    main()
