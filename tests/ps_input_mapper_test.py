#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    mapper = (ROOT / "src/ps/ps_input_mapper.cpp").read_text()

    assert "invertYAxis(state.left_stick_y)" in mapper
    assert "invertYAxis(state.right_stick_y)" in mapper
    assert "numeric_limits<int16_t>::min()" in mapper
    assert "numeric_limits<int16_t>::max()" in mapper
    assert "s.left_y = state.left_stick_y;" not in mapper
    assert "s.right_y = state.right_stick_y;" not in mapper
    assert "CHIAKI_CONTROLLER_BUTTON_TOUCHPAD" in mapper
    assert "touchpad.pressed || state.touchpad" in mapper
    assert "touchpad.touches.size()" in mapper
    assert "s.touches[i].id = touch_ids_[i]" in mapper
    assert "s.touches[i].x = touch.x" in mapper
    assert "s.touches[i].y = touch.y" in mapper

    reader = (ROOT / "src/ps/ps_touchpad_reader.cpp").read_text()
    controller = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
    assert "kTapMaxDistance = 10" in reader
    assert "kPanMinDistance = 30" in reader
    assert "milliseconds(200)" in reader
    assert "milliseconds(500)" in reader
    assert "milliseconds(150)" in reader
    assert "GestureState::Pending" in reader
    assert "GestureState::Pan" in reader
    assert "GestureState::LongPress" in reader
    assert "GestureState::ReleaseHold" in reader
    assert "CHIAKI_CONTROLLER_TOUCHES_MAX" in reader
    assert "updateTrackedTouches(state)" in reader
    assert "activeTouchCount() > 1" in reader
    assert "had_multiple_touches_" in reader
    assert "if (samples == 0)" in reader
    assert "currentState(false)" in reader
    assert "currentState(true)" in reader
    assert "const size_t count = state.count" in reader
    assert "blocked_until_release_" in reader
    assert "touchpad_reader_->read(input_suppressed)" in controller
    assert "input_mapper_->map(state, touchpad, &motion)" in controller
    assert "state = input_router_.route(state);" in controller
    assert "else if (input_suppressed_.load())" not in controller

    print("PS input mapper tests passed")


if __name__ == "__main__":
    main()
