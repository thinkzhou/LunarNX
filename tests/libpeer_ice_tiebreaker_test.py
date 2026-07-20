#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AGENT_SOURCE = ROOT / "lib/libpeer/src/agent.c"
AGENT_HEADER = ROOT / "lib/libpeer/src/agent.h"
TRACKED_PATCH = ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch"


def validate(source: str, header: str, label: str) -> None:
    assert "uint64_t tie_breaker;" in header, f"{label}: missing session tie-breaker"
    assert "agent->tie_breaker =" in source, f"{label}: tie-breaker is not generated"
    assert "if (agent->tie_breaker == 0)" in source, f"{label}: zero tie-breaker is not rejected"
    assert "agent->tie_breaker >>" in source, f"{label}: binding request does not reuse tie-breaker"
    assert "uint64_t tie_breaker = 0" not in source, f"{label}: fixed zero tie-breaker remains"


def main() -> None:
    validate(AGENT_SOURCE.read_text(), AGENT_HEADER.read_text(), "legacy libpeer")

    patch = TRACKED_PATCH.read_text()
    assert "+  uint64_t tie_breaker;" in patch, "tracked patch: missing session tie-breaker"
    assert "+  agent->tie_breaker =" in patch, "tracked patch: tie-breaker is not generated"
    assert "+  if (agent->tie_breaker == 0)" in patch, "tracked patch: zero guard missing"
    assert "+    stun_msg_write_attr(msg, STUN_ATTR_TYPE_ICE_CONTROLLING, 8, (char*)tie_breaker_network);" in patch
    assert "+    stun_msg_write_attr(msg, STUN_ATTR_TYPE_ICE_CONTROLLED, 8, (char*)tie_breaker_network);" in patch

    print("libpeer ICE tie-breaker tests passed")


if __name__ == "__main__":
    main()
