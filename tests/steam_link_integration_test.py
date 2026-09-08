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
    assert "src/steamlink/ihs_authorization.c" in makefile
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
    assert "--pair-code PAIRING_CODE" in desktop_probe
    assert "session/channels/video/ch_data_video.c" in desktop_build
    for override in ("ihs_discovery.c", "ihs_negotiation.c", "ihs_timer.c"):
        assert f"src/steamlink/{override}" in makefile
        assert f"src/steamlink/{override}" in desktop_build
    controller = read("src/steamlink/steam_link_stream_controller.cpp")
    assert "self->media_->hasVideoRecoveryRequest()" in controller
    assert "self->video_recovery_.reportLost(" in controller
    assert "media_activity_.expired(" in controller
    assert "pointer_.setVideoSize(" in controller
    assert "video_progress_.observe(" in controller
    assert "input_availability_.expired(" in controller
    assert 'persistentEventLog("steam-health"' in controller
    assert "successfulVideoPresentCount()" in controller
    assert "level <= IHS_LogLevelWarn || negotiation" in client

    print("steam link integration checks passed")


if __name__ == "__main__":
    main()
