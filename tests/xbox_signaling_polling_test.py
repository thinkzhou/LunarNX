#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    source = (ROOT / "src/app/xbox_session_client.cpp").read_text()
    header = (ROOT / "src/app/xbox_session_client.h").read_text()
    session = (ROOT / "src/app/xbox_stream_session.cpp").read_text()
    processor = (ROOT / "src/app/ice_candidate_processor.cpp").read_text()
    api = (ROOT / "src/api/xbox_api_client.cpp").read_text()
    transport = (ROOT / "src/app/web_rtc_transport.cpp").read_text()

    require("kSignalingPollInterval{1000}" in source,
            "signaling polling must use XStreaming's 1000ms cadence")
    require(source.count("sleep(kSignalingPollInterval)") == 2,
            "SDP and ICE retries must both use cancellable signaling sleep")
    require("std::this_thread::sleep_for" not in source,
            "ICE polling must not bypass cancellation")
    require(header.count("const SleepCallback& sleep") == 3,
            "session creation, SDP, and ICE must accept cancellable sleep")
    require("getIceCandidates(session_id, profile, cancel, sleep)" in session,
            "the stream session must pass its cancellable sleep callback")

    sdp_block = source[source.index("bool XboxSessionClient::exchangeSdpAnswer"):
                       source.index("bool XboxSessionClient::sendIceCandidates")]
    ice_block = source[source.index("XboxSessionClient::getIceCandidates"):
                       source.index("bool XboxSessionClient::keepAlive")]
    require(sdp_block.index("api_->getSdpAnswer") <
            sdp_block.index("sleep(kSignalingPollInterval)"),
            "SDP must perform its first GET before sleeping")
    require(ice_block.index("api_->getIceCandidates") <
            ice_block.index("sleep(kSignalingPollInterval)"),
            "ICE must perform its first GET before sleeping")
    require("parseRemotePayloads(raw_payloads, profile)" in ice_block,
            "ICE batches must be aggregated before candidate rewriting")
    require("quiet_polls >= 4" in ice_block and
            "hasRealCandidate(payload)" in ice_block,
            "ICE polling must wait for a real candidate and a quiet window")
    require("usernameFragment" in processor and
            "a=end-of-candidates" in processor,
            "local ICE payloads must include ufrag and an end marker")
    require('"/active"' in api and "cleanupActiveSessions" in api,
            "startup must support stale GSSV session cleanup")
    require("max_start_attempts = is_cloud ? 2 : 4" in session and
            "AgentCommandError" in session,
            "startup must retry cloud dead paths and home wake-up failures")
    require("kConnectedDataChannelTimeout{12}" in transport,
            "connected ICE without DTLS/SCTP must be detected as a dead media path")

    print("Xbox signaling polling tests passed")


if __name__ == "__main__":
    main()
