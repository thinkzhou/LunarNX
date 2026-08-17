from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-transport-diagnostics.patch").read_text()
KEY_PATCH = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-key-position-diagnostics.patch").read_text()
DOCKER_BUILD = (ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()
CONTAINER_BUILD = (ROOT / "tools/chiaki_switch/build_in_container.sh").read_text()
ADAPTER = (ROOT / "src/ps/chiaki_log_adapter.cpp").read_text()
SESSION = (ROOT / "src/ps/ps_stream_session.cpp").read_text()


def require(text: str, message: str) -> None:
    if text not in PATCH:
        raise AssertionError(message)


require("LUNARNX-PSRX packets=%llu bytes=%llu", "Takion aggregate throughput is missing")
require("proc_avg_us=%llu proc_max_us=%llu", "Takion processing timing is missing")
require("recv_gap_max_us=%llu recv_wait_max_us=%llu recv_timeout=%llu",
        "Takion receive gaps and timeout pressure are missing")
require("mac_fail=%llu av_drop=%llu reorder_skip=%llu alloc_fail=%llu",
        "Takion failure counters are missing")
require("video_q_high=%llu video_q_now=%llu", "Takion reorder depth is missing")
require("LUNARNX-PSVIDEO flush=%llu", "Video aggregate record is missing")
require("fec_ok=%llu fec_fail=%llu", "FEC outcome counters are missing")
require("callback_avg_us=%llu callback_max_us=%llu callback_fail=%llu",
        "Video callback timing is missing")
require(">= 10000000", "Diagnostics must be rate-limited to ten seconds")
require("#ifndef LUNARNX_CHIAKI_TRANSPORT_DIAG",
        "Diagnostics patch must define a safe default")
if PATCH.count("#if LUNARNX_CHIAKI_TRANSPORT_DIAG") < 8:
    raise AssertionError("Every transport hot path must compile out of release builds")

if "lunarnx-chiaki-transport-diagnostics.patch" not in CONTAINER_BUILD:
    raise AssertionError("Switch Chiaki build must apply the diagnostics patch")
if "lunarnx-chiaki-key-position-diagnostics.patch" not in CONTAINER_BUILD:
    raise AssertionError("Switch Chiaki build must apply key-position diagnostics")
if ('-e CHIAKI_TRANSPORT_DIAG="${CHIAKI_TRANSPORT_DIAG:-0}"' not in
        DOCKER_BUILD or
        'chiaki_transport_diag="${CHIAKI_TRANSPORT_DIAG:-0}"' not in
        CONTAINER_BUILD):
    raise AssertionError("Switch Chiaki transport diagnostics must default to disabled")
if "CHIAKI_TRANSPORT_DIAG must be 0 or 1" not in CONTAINER_BUILD:
    raise AssertionError("Switch Chiaki build must validate the diagnostics option")
if "-DLUNARNX_CHIAKI_TRANSPORT_DIAG=$chiaki_transport_diag" not in CONTAINER_BUILD:
    raise AssertionError("Switch Chiaki build must pass the diagnostics option to the compiler")
if "LUNARNX-PSKEY epoch_prev=%u epoch_next=%u" not in KEY_PATCH:
    raise AssertionError("Authenticated key-position epoch changes must be observable")
if "#if LUNARNX_CHIAKI_TRANSPORT_DIAG" not in KEY_PATCH:
    raise AssertionError("Key-position diagnostics must compile out of release builds")
if ("LUNARNX-PSRX " not in ADAPTER or "LUNARNX-PSVIDEO " not in ADAPTER or
        "LUNARNX-PSKEY " not in ADAPTER):
    raise AssertionError("Aggregate records must use the asynchronous diagnostic writer")
if "connect_info_.enable_idr_on_fec_failure = true;" not in SESSION:
    raise AssertionError("PS FEC failure must immediately request and wait for an IDR")

print("Chiaki transport diagnostics tests passed")
