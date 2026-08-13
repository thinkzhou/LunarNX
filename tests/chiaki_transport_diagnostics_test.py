from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-transport-diagnostics.patch").read_text()
KEY_PATCH = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-key-position-diagnostics.patch").read_text()
BUILD = (ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()
ADAPTER = (ROOT / "src/ps/chiaki_log_adapter.cpp").read_text()


def require(text: str, message: str) -> None:
    if text not in PATCH:
        raise AssertionError(message)


require("LUNARNX-PSRX packets=%llu bytes=%llu", "Takion aggregate throughput is missing")
require("proc_avg_us=%llu proc_max_us=%llu", "Takion processing timing is missing")
require("mac_fail=%llu av_drop=%llu reorder_skip=%llu alloc_fail=%llu",
        "Takion failure counters are missing")
require("video_q_high=%llu video_q_now=%llu", "Takion reorder depth is missing")
require("LUNARNX-PSVIDEO flush=%llu", "Video aggregate record is missing")
require("fec_ok=%llu fec_fail=%llu", "FEC outcome counters are missing")
require("callback_avg_us=%llu callback_max_us=%llu callback_fail=%llu",
        "Video callback timing is missing")
require(">= 10000000", "Diagnostics must be rate-limited to ten seconds")

if "lunarnx-chiaki-transport-diagnostics.patch" not in BUILD:
    raise AssertionError("Switch Chiaki build must apply the diagnostics patch")
if "lunarnx-chiaki-key-position-diagnostics.patch" not in BUILD:
    raise AssertionError("Switch Chiaki build must apply key-position diagnostics")
if "LUNARNX-PSKEY epoch_prev=%u epoch_next=%u" not in KEY_PATCH:
    raise AssertionError("Authenticated key-position epoch changes must be observable")
if ("LUNARNX-PSRX " not in ADAPTER or "LUNARNX-PSVIDEO " not in ADAPTER or
        "LUNARNX-PSKEY " not in ADAPTER):
    raise AssertionError("Aggregate records must use the asynchronous diagnostic writer")

print("Chiaki transport diagnostics tests passed")
