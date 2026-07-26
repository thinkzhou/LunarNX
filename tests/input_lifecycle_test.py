#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    reader = Path("src/input/gamepad_reader.cpp").read_text()
    rumble = Path("src/input/rumble_controller.cpp").read_text()
    session = Path("src/app/xbox_stream_session.cpp").read_text()

    initialize_start = reader.index("bool GamepadReader::initialize()")
    initialize_end = reader.index("GamepadState GamepadReader::read()")
    initialize = reader[initialize_start:initialize_end]
    require("delete static_cast<PadState*>(pad_state_);" in initialize,
            "initialize must delete an old PadState")
    require("pad_state_ = nullptr;" in initialize,
            "initialize must clear the old PadState pointer")

    reset_pos = session.index("xinput_.reset();")
    metadata_pos = session.index("encodeMetadata(0)")
    require(reset_pos < metadata_pos,
            "encoder reset must happen before metadata")

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
