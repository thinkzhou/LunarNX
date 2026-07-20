#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AGENT_SOURCE = ROOT / "lib/libpeer/src/agent.c"
TRACKED_PATCH = ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch"


def validate(text: str, label: str) -> None:
    assert "uint32_t priority_network = htonl(pair->local->priority);" in text, (
        f"{label}: ICE PRIORITY is not encoded in network byte order"
    )
    assert "uint8_t tie_breaker_network[8];" in text, (
        f"{label}: ICE role tie-breaker has no network-order representation"
    )
    assert "tie_breaker_network[i] =" in text, (
        f"{label}: ICE role tie-breaker bytes are not encoded"
    )
    assert "(char*)&priority_network" in text, (
        f"{label}: STUN PRIORITY does not use encoded value"
    )
    assert "(char*)tie_breaker_network" in text, (
        f"{label}: ICE role attribute does not use encoded value"
    )
    assert "(char*)&agent->nominated_pair->priority" not in text, (
        f"{label}: candidate-pair priority is still sent as STUN PRIORITY"
    )


def main() -> None:
    validate(AGENT_SOURCE.read_text(), "legacy libpeer")
    patch = TRACKED_PATCH.read_text()
    added_lines = "\n".join(
        line[1:] for line in patch.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )
    validate(added_lines, "tracked legacy patch")
    print("libpeer ICE binding encoding tests passed")


if __name__ == "__main__":
    main()
