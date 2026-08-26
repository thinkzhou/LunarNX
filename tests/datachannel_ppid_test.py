#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    peer_manager = (ROOT / "src/webrtc/peer_manager.cpp").read_text()
    peer_connection_h = (ROOT / "lib/libpeer/src/peer_connection.h").read_text()
    peer_connection_c = (ROOT / "lib/libpeer/src/peer_connection.c").read_text()

    assert "peer_connection_datachannel_send_sid_binary" in peer_connection_h
    assert "peer_connection_datachannel_send_sid_binary" in peer_connection_c
    assert "PPID_BINARY" in peer_connection_c

    input_method_start = peer_manager.index("bool PeerManager::sendInputData")
    input_method_end = peer_manager.index("bool PeerManager::sendControlData")
    input_method = peer_manager[input_method_start:input_method_end]
    assert "enqueueData(OutboundType::InputReliable" in input_method
    assert "sendLatestInputData" in input_method
    assert "sendTransitionInputData" in input_method
    assert "enqueueData(OutboundType::InputTransition" in input_method
    assert "kInputTransitionTtl" in input_method
    outbound_start = peer_manager.index("int PeerManager::sendOutboundCommand")
    outbound_end = peer_manager.index("void PeerManager::drainOutboundCommands")
    outbound_method = peer_manager[outbound_start:outbound_end]
    assert "peer_connection_datachannel_send_sid_binary" in outbound_method

    print("Datachannel PPID tests passed")


if __name__ == "__main__":
    main()
