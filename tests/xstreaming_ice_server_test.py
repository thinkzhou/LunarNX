#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PEER_MANAGER = ROOT / "src/webrtc/peer_manager.cpp"
PEER_HEADER = ROOT / "lib/libpeer/src/peer_connection.h"
AGENT_SOURCE = ROOT / "lib/libpeer/src/agent.c"
TRACKED_PATCH = ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch"

XSTREAMING_STUN_SERVERS = (
    "stun:worldaz.relay.teams.microsoft.com:3478",
    "stun:stun.l.google.com:19302",
    "stun:stun1.l.google.com:19302",
    "stun:relay1.expressturn.com",
    "stun:relay2.expressturn.com",
    "stun:stun.kinesisvideo.us-east-1.amazonaws.com:443",
    "stun:stun.douyucdn.cn:18000",
)


def validate_libpeer(header: str, agent: str, label: str) -> None:
    assert "IceServer ice_servers[7];" in header, (
        f"{label}: PeerConfiguration cannot hold XStreaming's STUN list"
    )
    assert "port = 3478;" in agent, (
        f"{label}: STUN URLs without an explicit port do not use port 3478"
    )
    assert "agent_has_candidate_address" in agent, (
        f"{label}: duplicate server-reflexive candidates are published"
    )


def main() -> None:
    manager = PEER_MANAGER.read_text()
    for index, server in enumerate(XSTREAMING_STUN_SERVERS):
        expected = f'config.ice_servers[{index}].urls = "{server}";'
        assert expected in manager, f"missing XStreaming ICE server: {server}"

    validate_libpeer(PEER_HEADER.read_text(), AGENT_SOURCE.read_text(),
                     "legacy libpeer")
    patch = TRACKED_PATCH.read_text()
    added_lines = "\n".join(
        line[1:]
        for line in patch.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )
    validate_libpeer(added_lines, added_lines, "tracked legacy patch")
    print("XStreaming ICE server tests passed")


if __name__ == "__main__":
    main()
