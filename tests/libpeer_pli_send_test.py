#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    peer = Path("lib/libpeer/src/peer_connection.c").read_text()
    dtls = Path("lib/libpeer/src/dtls_srtp.c").read_text()
    dtls_header = Path("lib/libpeer/src/dtls_srtp.h").read_text()
    tracked_patch = Path(
        "tools/libpeer_legacy/legacy-libpeer-switch.patch"
    ).read_text()
    manager = Path("src/webrtc/peer_manager.cpp").read_text()
    transport = Path("src/app/web_rtc_transport.cpp").read_text()
    channels = Path("src/app/xbox_channel_manager.cpp").read_text()

    pli_start = peer.index("int peer_connection_send_rtcp_pil")
    pli_body = peer[pli_start:peer.index("// callbacks", pli_start)]
    require("return srtp_protect_rtcp" in dtls,
            "SRTCP protection errors must be returned to the caller")
    require("size_t packet_capacity" in dtls_header and
            "packet_capacity - (size_t)*bytes < SRTP_MAX_TRAILER_LEN + 4" in dtls,
            "the SRTCP boundary must reject buffers without trailer capacity")
    require("PeerManager::requestVideoKeyframe" in manager and
            "WebRtcTransport::requestVideoKeyframe" in transport,
            "The app transport must expose the libpeer PLI sender")
    require("transport_.requestVideoKeyframe()" in channels,
            "Xbox keyframe requests must include the RTCP PLI used by libwebrtc")
    require("peer_connection_send_receiver_feedback" in peer,
            "libpeer must expose periodic receiver feedback for bitrate ramp-up")
    require("int peer_connection_send_nack" in peer and
            "header->type = RTCP_RTPFB" in peer and
            "network_pid = htons(pid)" in peer and
            "network_blp = htons(blp)" in peer,
            "libpeer must send network-order RFC 4585 Generic NACK feedback")
    rtcp_send_start = peer.index("static int peer_connection_send_rtcp_packet")
    rtcp_send_body = peer[rtcp_send_start:peer.index("static int peer_connection_send_remb", rtcp_send_start)]
    require("SRTP_MAX_TRAILER_LEN + 4" in rtcp_send_body and
            "CONFIG_PACKET_BUFFER_SIZE" in rtcp_send_body and
            "memcpy" in rtcp_send_body,
            "all outbound RTCP must be copied into an aligned buffer with libSRTP trailer capacity")
    require("dtls_srtp_encrypt_rctp_packet" in rtcp_send_body and
            "agent_send(&pc->agent" in rtcp_send_body,
            "the capacity-checked RTCP helper must protect and send the copied packet")
    require("sizeof(protected_packet.bytes)" in rtcp_send_body,
            "the RTCP helper must pass the actual scratch-buffer capacity")
    require("return peer_connection_send_rtcp_packet(pc, plibuf, size);" in pli_body,
            "PLI must use the shared capacity-checked SRTCP send helper")
    require("peer_connection_send_receiver_feedback_stats" in peer,
            "receiver feedback must accept loss repaired by the application jitter buffer")
    require("goog-remb" in Path("lib/libpeer/src/sdp.c").read_text(),
            "SDP must negotiate goog-remb before sending REMB")
    require("dlsr = htonl(delta_ms << 16)" not in peer,
            "feedback implementation should not carry the old milliseconds/seconds bug")
    require("(uint64_t)delta_ms << 16" in peer and "/ 1000" in peer,
            "RTCP DLSR must convert milliseconds to 1/65536 seconds")
    require("SRTP_MAX_TRAILER_LEN + 4" in tracked_patch and
            "sizeof(protected_packet.bytes)" in tracked_patch and
            "size_t packet_capacity" in tracked_patch,
            "tracked legacy patch must reproduce the SRTCP overflow guard")

    print("libpeer PLI send tests passed")


if __name__ == "__main__":
    main()
