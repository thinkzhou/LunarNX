#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    socket_header = (ROOT / "lib/libpeer/src/socket.h").read_text()
    socket_source = (ROOT / "lib/libpeer/src/socket.c").read_text()
    agent_header = (ROOT / "lib/libpeer/src/agent.h").read_text()
    agent_source = (ROOT / "lib/libpeer/src/agent.c").read_text()
    peer_source = (ROOT / "lib/libpeer/src/peer_connection.c").read_text()
    tracked_patch = (
        ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch"
    ).read_text()

    require("udp_socket_recvfrom_nonblocking" in socket_header,
            "UDP sockets must expose a completed-state nonblocking receive")
    require("MSG_DONTWAIT" in socket_source and
            "EAGAIN" in socket_source and
            "EWOULDBLOCK" in socket_source,
            "Nonblocking receive must stop quietly when the socket is empty")
    require("agent_recv_nonblocking" in agent_header and
            "int* datagram_received" in agent_header,
            "Agent receive must report every raw UDP datagram against the budget")

    recv_start = agent_source.index("int agent_recv(")
    recv_end = agent_source.index("void agent_set_remote_description")
    recv_impl = agent_source[recv_start:recv_end]
    require("agent_recv_one" in recv_impl and "attempts < 8" in recv_impl,
            "Timed handshake receive must retain bounded transparent STUN handling")
    require("int agent_recv_nonblocking" in recv_impl,
            "Completed-state receive must process one raw datagram without polling")

    dtls_recv_start = peer_source.index("static int peer_connection_dtls_srtp_recv")
    dtls_recv_end = peer_source.index("static int peer_connection_dtls_srtp_send")
    dtls_recv = peer_source[dtls_recv_start:dtls_recv_end]
    require("agent_recv(&pc->agent" in dtls_recv,
            "DTLS handshake receive must retain its timed polling path")

    loop_start = peer_source.index("int peer_connection_loop")
    completed_start = peer_source.index("case PEER_CONNECTION_COMPLETED", loop_start)
    completed_end = peer_source.index("case PEER_CONNECTION_FAILED", completed_start)
    completed = peer_source[completed_start:completed_end]
    require("agent_recv_nonblocking(&pc->agent" in completed,
            "Completed-state receive must not poll before every UDP datagram")
    require("agent_recv(&pc->agent" not in completed,
            "Completed-state receive must never use the timed receive path")
    require("PEER_CONNECTION_RECEIVE_BUDGET_US 1000" in peer_source and
            "PEER_CONNECTION_MAX_PACKETS_PER_LOOP 128" in peer_source,
            "Completed-state receive must have both time and datagram budgets")
    require("datagram_received" in completed and
            "packets_processed++" in completed,
            "STUN, DTLS, RTCP, and RTP datagrams must all consume packet budget")

    for token in (
        "udp_socket_recvfrom_nonblocking",
        "MSG_DONTWAIT",
        "agent_recv_nonblocking",
        "PEER_CONNECTION_RECEIVE_BUDGET_US 1000",
    ):
        require(token in tracked_patch,
                f"Tracked legacy patch must preserve {token}")

    print("libpeer nonblocking receive tests passed")


if __name__ == "__main__":
    main()
