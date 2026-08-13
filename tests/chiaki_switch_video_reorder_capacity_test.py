#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
PATCH = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-video-reorder-capacity.patch").read_text()
BUILD = (ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


match = re.search(
    r"#ifdef __SWITCH__.*?"
    r"TAKION_AV_VIDEO_REORDER_QUEUE_SIZE_EXP\s+(\d+).*?"
    r"#else.*?TAKION_AV_VIDEO_REORDER_QUEUE_SIZE_EXP\s+(\d+)",
    PATCH,
    re.DOTALL,
)
require(match is not None, "patch must define separate Switch and upstream windows")
switch_exp, upstream_exp = map(int, match.groups())
switch_capacity = 1 << switch_exp
upstream_capacity = 1 << upstream_exp

# The hardware trace contains an 88+6-unit IDR. With packet zero delayed, all
# other units must fit in the reorder queue until its 16 ms timer expires.
observed_frame_units = 88 + 6
require(upstream_capacity < observed_frame_units,
        "regression fixture must exceed the upstream 64-packet window")
require(switch_capacity >= observed_frame_units * 2,
        "Switch window must retain the observed frame with burst headroom")
require(switch_capacity <= 256,
        "keep the Switch-only dynamic allocation bounded")
require(
    'git -C "$src" apply /work/tools/chiaki_switch/'
    'lunarnx-chiaki-video-reorder-capacity.patch' in BUILD,
    "Switch Chiaki build must apply the video reorder-capacity patch",
)

print(
    "Chiaki Switch video reorder capacity test passed "
    f"({upstream_capacity} -> {switch_capacity}, observed={observed_frame_units})"
)
