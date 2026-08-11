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

    print("PS input mapper tests passed")


if __name__ == "__main__":
    main()
