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

    print("Xbox signaling polling tests passed")


if __name__ == "__main__":
    main()
