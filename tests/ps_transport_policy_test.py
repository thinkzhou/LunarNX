#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


session = Path("src/ps/ps_stream_session.cpp").read_text()

require("connect_info_.enable_dualsense = is_ps5_;" in session,
        "PS4 must advertise DualShock 4 while PS5 keeps DualSense support")
require("connect_info_.packet_loss_max = 0.05;" in session,
        "PS transport must use Chiaki's 5% reported-loss ceiling")
require("connect_info_.enable_idr_on_fec_failure = true;" in session,
        "Chiaki must own IDR recovery when video FEC genuinely fails")

print("PS transport policy tests passed")
