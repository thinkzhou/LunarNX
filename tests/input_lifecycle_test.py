#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    reader = Path("src/input/gamepad_reader.cpp").read_text()
    rumble = Path("src/input/rumble_controller.cpp").read_text()
    session = Path("src/app/xbox_stream_session.cpp").read_text()
    encoder = Path("src/input/xinput_encoder.h").read_text()
    peer = Path("src/webrtc/peer_manager.cpp").read_text()

    initialize_start = reader.index("bool GamepadReader::initialize()")
    initialize_end = reader.index("GamepadState GamepadReader::read()")
    initialize = reader[initialize_start:initialize_end]
    require("delete static_cast<PadState*>(pad_state_);" in initialize,
            "initialize must delete an old PadState")
    require("pad_state_ = nullptr;" in initialize,
            "initialize must clear the old PadState pointer")

    require("xinput_.reset()" not in session,
            "input sequence lifecycle must not be tied to the encoder")
    require("sequence_" not in encoder and "void reset()" not in encoder,
            "XInputEncoder must remain stateless")
    initialize_start = peer.index("bool PeerManager::initialize()")
    initialize_end = peer.index("void PeerManager::setCallbacks", initialize_start)
    require("next_input_sequence_ = 0;" in peer[initialize_start:initialize_end],
            "a new PeerManager association must reset input sequence to zero")
    require(peer.count("++next_input_sequence_;") == 1 and
            "void PeerManager::commitSequencedInputResult(int result)" in peer,
            "input sequence may advance only in the send-result commit helper")
    commit_start = peer.index("void PeerManager::commitSequencedInputResult")
    commit_end = peer.index("int PeerManager::sendInputCommand", commit_start)
    require("if (result >= 0)" in peer[commit_start:commit_end],
            "failed input sends must not consume a sequence")

    rumble_start = rumble.index("bool RumbleController::initialize()")
    rumble_end = rumble.index("void RumbleController::setRumble")
    initialize_rumble = rumble[rumble_start:rumble_end]
    probe_pos = initialize_rumble.index("hidInitializeVibrationDevices")
    require("stop();" in initialize_rumble,
            "rumble initialize must stop old output before probing handles")
    require("handheld_device_ = {};" in initialize_rumble,
            "rumble initialize must clear the handheld vibration device")
    require("player_devices_ = {};" in initialize_rumble,
            "rumble initialize must clear player vibration devices")
    require(initialize_rumble.index("stop();") < probe_pos,
            "rumble initialize must stop old output before probing handles")
    require(initialize_rumble.index("handheld_device_ = {};") < probe_pos,
            "rumble initialize must clear handheld handles before probing")
    require(initialize_rumble.index("player_devices_ = {};") < probe_pos,
            "rumble initialize must clear player handles before probing")

    print("Input lifecycle tests passed")


if __name__ == "__main__":
    main()
