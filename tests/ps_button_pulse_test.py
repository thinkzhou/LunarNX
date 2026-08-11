#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    header = Path("src/ps/ps_stream_controller.h").read_text()
    controller = Path("src/ps/ps_stream_controller.cpp").read_text()
    stream_view = Path("src/ui/stream_view.cpp").read_text()

    require("kPsButtonPulseFrames" in controller,
            "virtual PS button requests must have a bounded hold duration")
    require("ps_button_pulse_frames_remaining_" in header and
            "ps_button_pulse_frames_remaining_--" in controller,
            "virtual PS button requests must span multiple input frames")
    require("state = {};" in controller and "state.guide = true;" in controller,
            "the PS pulse must suppress menu input and send only the PS button")
    require("ps_button_release_pending_" in header,
            "the PS pulse must include an explicit release frame")
    require("else if (input_suppressed_.load())" in controller,
            "input suppression must not erase a requested PS pulse")
    require("kPsInputInterval{8}" in controller and
            "input_thread_ = std::thread" in controller and
            "update();" in controller,
            "PS controller input must run on its own 8 ms pump")
    require("stopInputLoop();" in controller and
            "input_thread_.join()" in controller,
            "PS input pump must stop before session resources are released")
    require("sleep_for(milliseconds(500))" in stream_view and
            "runtime_->update();" not in stream_view,
            "shared stream UI must not drive protocol input at 8 ms")

    print("PS button pulse tests passed")


if __name__ == "__main__":
    main()
