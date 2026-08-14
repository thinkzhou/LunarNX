#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    reader = (ROOT / "src/ps/ps_motion_reader.cpp").read_text()
    mapper = (ROOT / "src/ps/ps_input_mapper.cpp").read_text()
    controller = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
    makefile = (ROOT / "Makefile.switch").read_text()

    assert "hidGetSixAxisSensorHandles" in reader
    assert "hidStartSixAxisSensor" in reader
    assert "hidStopSixAxisSensor" in reader
    assert "hidGetSixAxisSensorStates" in reader
    assert "return impl_->last_state" in reader
    assert "impl_->last_state = convertSixAxis(sixaxis)" in reader
    assert "angular_velocity.x * kTau" in reader
    assert "state.accel_x = -sixaxis.acceleration.x" in reader
    assert "state.accel_y = -sixaxis.acceleration.z" in reader
    assert "state.accel_z = sixaxis.acceleration.y" in reader
    assert "sixaxis.direction.direction" in reader
    assert "motion && motion->valid" in mapper
    assert "s.gyro_x = motion->gyro_x" in mapper
    assert "s.orient_w = motion->orient_w" in mapper
    assert "motion_reader_->read(input_suppressed)" in controller
    assert "src/ps/ps_motion_reader.cpp" in makefile
    print("PS motion input tests passed")


if __name__ == "__main__":
    main()
