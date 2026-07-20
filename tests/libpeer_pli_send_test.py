#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    peer = Path("lib/libpeer/src/peer_connection.c").read_text()
    dtls = Path("lib/libpeer/src/dtls_srtp.c").read_text()
    manager = Path("src/webrtc/peer_manager.cpp").read_text()
    transport = Path("src/app/web_rtc_transport.cpp").read_text()
    channels = Path("src/app/xbox_channel_manager.cpp").read_text()

    pli_start = peer.index("int peer_connection_send_rtcp_pil")
    pli_body = peer[pli_start:peer.index("// callbacks", pli_start)]
    require("dtls_srtp_encrypt_rctp_packet" in pli_body,
            "PLI must be protected with the negotiated outbound SRTCP context")
    require("agent_send(&pc->agent" in pli_body,
            "Protected PLI must be sent through the selected ICE pair")
    require("return srtp_protect_rtcp" in dtls,
            "SRTCP protection errors must be returned to the caller")
    require("PeerManager::requestVideoKeyframe" in manager and
            "WebRtcTransport::requestVideoKeyframe" in transport,
            "The app transport must expose the libpeer PLI sender")
    require("transport_.requestVideoKeyframe()" in channels,
            "Xbox keyframe requests must include the RTCP PLI used by libwebrtc")

    print("libpeer PLI send tests passed")


if __name__ == "__main__":
    main()
