#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    sdp = (ROOT / "lib/libpeer/src/sdp.c").read_text()
    peer = (ROOT / "lib/libpeer/src/peer_connection.c").read_text()
    ice = (ROOT / "lib/libpeer/src/ice.c").read_text()
    agent = (ROOT / "lib/libpeer/src/agent.c").read_text()
    stun_header = (ROOT / "lib/libpeer/src/stun.h").read_text()
    stun_source = (ROOT / "lib/libpeer/src/stun.c").read_text()
    stream_session = (ROOT / "src/app/xbox_stream_session.cpp").read_text()
    patch = (ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch").read_text()
    patch_added = "\n".join(
        line[1:]
        for line in patch.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )

    require('"a=group:BUNDLE 0 1 2"' in sdp,
            "offer BUNDLE mids must match XStreaming's numeric mids")
    require('"a=mid:0"' in sdp and '"a=mid:1"' in sdp and '"a=mid:2"' in sdp,
            "offer media sections must use numeric mids")
    require('"a=ice-options:trickle renomination"' in sdp,
            "offer must advertise the trickle ICE behavior it uses")
    require("if (sdp_type == SDP_TYPE_ANSWER)" in peer and
            "sdp_append(pc->sdp, description);" in peer,
            "offer candidates must not be appended inline")
    require("pc->onicecandidate(description, pc->config.user_data);" in peer,
            "gathered candidates must still be emitted for the /ice payload")

    require("ice_candidate_set_related_address" in ice,
            "srflx candidates must support a real related host address")
    require("agent_find_host_candidate" in agent and
            "ice_candidate_set_related_address" in agent,
            "STUN gathering must attach the host base to srflx candidates")
    require(agent.count("recv_msg.size = (size_t)ret;") >= 3,
            "every synchronous STUN/TURN receive must expose its datagram length before parsing")
    require("msg.size = size;" in stun_source,
            "STUN integrity validation must parse only the received datagram length")
    require("agent_stun_transaction_matches" in agent,
            "synchronous STUN gathering must reject responses for another transaction")
    require("agent_address_is_usable" in agent and
            "agent_address_is_usable(&recv_msg.mapped_addr)" in agent,
            "STUN gathering must reject missing or zero mapped addresses")
    require("AGENT_STUN_POLL_TIMEOUT_MS 50" in agent and
            "AGENT_STUN_RECV_MAXTIMES 20" in agent,
            "synchronous STUN gathering must avoid 1000 one-millisecond BSD HLE polls")
    require("agent_socket_recv_with_timeout" in agent,
            "STUN gathering needs a longer poll without slowing the media event loop")

    for field in ("priority", "use_candidate", "ice_controlling", "ice_controlled"):
        require(field in stun_header,
                f"parsed STUN messages must retain {field}")
    require("STUN_ATTR_TYPE_PRIORITY" in stun_source and
            "msg->priority" in stun_source,
            "STUN PRIORITY must be parsed")
    require("STUN_ATTR_TYPE_USE_CANDIDATE" in stun_source and
            "msg->use_candidate" in stun_source,
            "STUN USE-CANDIDATE must be parsed")
    require("agent_add_remote_peer_reflexive_candidate" in agent,
            "unknown STUN sources must create peer-reflexive candidates")
    require("pair->triggered" in agent,
            "incoming Binding requests must schedule triggered checks")
    require("agent_handle_role_conflict" in agent,
            "incoming ICE role conflicts must be handled")
    require("msg->use_candidate" in agent and "remote_nomination_pending" in agent,
            "remote USE-CANDIDATE nomination must be retained")

    require("AGENT_CHECK_MAX_RETRANSMISSIONS 6" in agent,
            "ICE checks must use libjuice's six retransmissions")
    require("AGENT_CHECK_MAX_RTO_MS 8000" in agent,
            "ICE exponential backoff must cap at eight seconds")
    require("kDataChannelTimeout{45}" in stream_session,
            "the app must not stop ICE before the RFC-style retry window")

    for token in (
        '"a=group:BUNDLE 0 1 2"',
        '"a=ice-options:trickle renomination"',
        "pc->onicecandidate(description, pc->config.user_data);",
        "ice_candidate_set_related_address",
        "recv_msg.size = (size_t)ret;",
        "msg.size = size;",
        "agent_stun_transaction_matches",
        "agent_address_is_usable",
        "AGENT_STUN_POLL_TIMEOUT_MS 50",
        "agent_socket_recv_with_timeout",
        "agent_add_remote_peer_reflexive_candidate",
        "pair->triggered",
        "agent_handle_role_conflict",
        "msg->use_candidate",
        "AGENT_CHECK_MAX_RETRANSMISSIONS 6",
        "AGENT_CHECK_MAX_RTO_MS 8000",
    ):
        require(token in patch_added,
                f"tracked legacy patch must preserve {token}")

    print("XStreaming ICE alignment tests passed")


if __name__ == "__main__":
    main()
