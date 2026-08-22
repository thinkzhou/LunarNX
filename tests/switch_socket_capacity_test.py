#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    wrapper = (ROOT / "src/platform/switch_wrapper.c").read_text()
    socket_policy = wrapper.split("AppletType at", 1)[1].split("#ifdef DEBUG", 1)[0]

    assert socket_policy.count("cfg.num_bsd_sessions = 12;") == 2, (
        "Application and LibraryApplet paths must both keep enough BSD "
        "sessions for Chiaki's stop socket and worker sockets"
    )
    assert "cfg.num_bsd_sessions = 2;" not in socket_policy
    assert "cfg.udp_rx_buf_size = 512 * 1024;" in socket_policy
    assert (
        '"socketInitialize application enhanced rc=0x%08x udp_rx=%u '
        'sessions=%u efficiency=%u"' in socket_policy
    )
    assert (
        '"socketInitialize applet enhanced rc=0x%08x udp_rx=%u '
        'sessions=%u efficiency=%u"' in socket_policy
    )
    assert socket_policy.count(
        "rc, cfg.udp_rx_buf_size, cfg.num_bsd_sessions, cfg.sb_efficiency"
    ) == 2

    print("Switch socket capacity tests passed")


if __name__ == "__main__":
    main()
