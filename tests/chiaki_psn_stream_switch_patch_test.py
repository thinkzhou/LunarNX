#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-stream-switch.patch").read_text()
BUILD = (ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


require("chiaki_session_get_stream_connection_switch_received" in PATCH,
        "Senkusha must observe the synchronized stream-switch state")
require("waking Senkusha" in PATCH and "active=%d" in PATCH,
        "early switch ACK diagnostic must identify whether Senkusha is active")
require("Senkusha interrupted by early stream switch ACK" in PATCH,
        "early switch ACK must bypass the fixed BANG timeout")
require("senkusha->state == STATE_EXPECT_BANG" not in PATCH,
        "early switch ACK must interrupt Takion connect/cookie waits too")
require("state=%d; skipping remaining probes" in PATCH,
        "early switch diagnostic must expose the interrupted Senkusha state")
require(PATCH.count("senkusha_stream_switch_interrupted(senkusha)") >= 7,
        "every Senkusha connect, protocol, BANG, RTT and MTU wait must "
        "explicitly propagate an early stream-switch interruption")
require("early stream switch ACK; using safe MTU fallback" in PATCH and
        "err = CHIAKI_ERR_UNKNOWN;" in PATCH,
        "an early ACK must trigger the existing fragmentation-safe fallback "
        "instead of reporting unmeasured Senkusha values as successful")
require("skipping duplicate switch request" in PATCH,
        "session must not send a second switch request after an early ACK")
require("initial seq num %#x" in PATCH and "protobuf_type=%d" in PATCH,
        "Takion diagnostics must expose both sequence values and dropped payload type")
require("git -C \"$src\" apply /work/tools/chiaki_switch/lunarnx-chiaki-stream-switch.patch" in BUILD,
        "Switch SDK build must apply the tracked stream-switch patch")

print("Chiaki PSN stream-switch patch test passed")
