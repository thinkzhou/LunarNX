from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-packetstats-wrap.patch").read_text()
BUILD = (ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()


def require(text: str, message: str) -> None:
    if text not in PATCH:
        raise AssertionError(message)


require("chiaki_mutex_lock(&stats->mutex);", "sequence updates must share the stats mutex")
require("(ChiakiSeqNum16)(stats->seq_max - stats->seq_min)",
        "sequence difference must wrap in the 16-bit domain")
require("seq_diff > stats->seq_received ? seq_diff - stats->seq_received : 0",
        "duplicate or reordered packets must not become synthetic loss")

if "lunarnx-chiaki-packetstats-wrap.patch" not in BUILD:
    raise AssertionError("Switch Chiaki build must apply the packet-stats patch")


def seq_window(start: int, end: int, received: int) -> tuple[int, int]:
    expected = (end - start) & 0xFFFF
    lost = expected - received if expected > received else 0
    return expected, lost


assert seq_window(100, 110, 10) == (10, 0)
assert seq_window(0xFFFA, 4, 10) == (10, 0)
assert seq_window(0xFFFA, 4, 8) == (10, 2)
assert seq_window(100, 110, 12) == (10, 0)

print("Chiaki packet-stats wrap test passed")
