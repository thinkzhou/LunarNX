#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AGENT_SOURCE = ROOT / "lib/libpeer/src/agent.c"
AGENT_HEADER = ROOT / "lib/libpeer/src/agent.h"
ICE_HEADER = ROOT / "lib/libpeer/src/ice.h"
TRACKED_PATCH = ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch"


def validate(agent: str, ice: str, label: str) -> None:
    assert "agent_candidate_pair_priority" in agent, (
        f"{label}: candidate-pair priority does not follow the ICE formula"
    )
    assert "agent_schedule_connectivity_checks" in agent, (
        f"{label}: candidate pairs are still checked serially"
    )
    assert "agent_find_pair_by_transaction" in agent, (
        f"{label}: concurrent STUN responses cannot be matched to candidate pairs"
    )
    assert "pair->transaction_id[0] = (uint32_t)rand();" in agent, (
        f"{label}: candidate pairs still share libpeer's fixed STUN transaction ID"
    )
    assert "nomination_pending" in ice, (
        f"{label}: candidate pairs cannot represent regular nomination"
    )
    assert "nominated" in ice, (
        f"{label}: candidate pairs cannot record nomination completion"
    )
    assert "use_candidate" in agent, (
        f"{label}: STUN requests cannot distinguish checks from nomination"
    )
    assert "if (use_candidate)" in agent, (
        f"{label}: USE-CANDIDATE is still sent on every connectivity check"
    )
    assert "if (agent->candidate_pairs_num == 0)" in agent, (
        f"{label}: an empty checklist fails before trickle ICE candidates arrive"
    )


def main() -> None:
    validate(AGENT_SOURCE.read_text(), ICE_HEADER.read_text(), "legacy libpeer")
    assert "#define AGENT_MAX_CANDIDATES 16" in AGENT_HEADER.read_text(), (
        "legacy libpeer truncates valid Xbox remote ICE candidates"
    )

    patch = TRACKED_PATCH.read_text()
    added_lines = "\n".join(
        line[1:]
        for line in patch.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )
    validate(added_lines, added_lines, "tracked legacy patch")
    assert "#define AGENT_MAX_CANDIDATES 16" in added_lines, (
        "tracked legacy patch does not preserve the 16-candidate limit"
    )
    print("libpeer ICE checklist tests passed")


if __name__ == "__main__":
    main()
