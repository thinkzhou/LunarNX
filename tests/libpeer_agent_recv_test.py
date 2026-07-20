#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    agent = (ROOT / "lib/libpeer/src/agent.c").read_text()
    recv_start = agent.index("int agent_recv")
    recv_end = agent.index("void agent_set_remote_description")
    recv_impl = agent[recv_start:recv_end]

    assert "attempts < 8" in recv_impl
    assert "stun_probe(buf, ret) != 0" in recv_impl
    assert "agent_process_stun_request" in recv_impl
    assert "agent_process_stun_response" in recv_impl

    assert "AGENT_ENABLE_DEBUG_LOGS" in agent
    assert "agent_ice_debug_log" in agent
    assert "[DEBUG-ice]" in agent
    assert "conncheck send" in agent
    assert "candidate pair index=" in agent
    assert "candidate select index=" in agent
    assert "debug_send_logs < 64" in agent
    assert "debug_recv_logs < 64" in agent
    assert "for (i = 0; i < 2; i++)" not in agent
    assert "sizeof(addr_type) / sizeof(addr_type[0])" in agent
    header = (ROOT / "lib/libpeer/src/agent.h").read_text()
    assert "debug_pair_logs" in header
    assert "debug_select_logs" in header
    assert "debug_stun_logs" in header

    print("libpeer agent recv tests passed")


if __name__ == "__main__":
    main()
