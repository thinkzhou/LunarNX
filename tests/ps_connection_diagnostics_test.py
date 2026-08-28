#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


makefile = (ROOT / "Makefile.switch").read_text()
trace_h = (ROOT / "src/ps/ps_connection_trace.h").read_text()
trace_cpp = (ROOT / "src/ps/ps_connection_trace.cpp").read_text()
controller = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
remote = (ROOT / "src/ps/ps_remote_connector.cpp").read_text()
session = (ROOT / "src/ps/ps_stream_session.cpp").read_text()
bridge = (ROOT / "src/ps/ps_media_bridge.cpp").read_text()
ui = (ROOT / "src/ui/ps_activity.cpp").read_text()
manager = (ROOT / "src/ps/ps_manager.cpp").read_text()

require(
    "src/ps/ps_connection_trace.cpp" in makefile,
    "the Switch build must include the PS connection trace",
)
require(
    "kMaxEntries" in trace_h and "std::array<Entry" in trace_h,
    "connection diagnostics must use bounded memory",
)
record_body = trace_cpp.split("void PsConnectionTrace::record", 1)[1].split(
    "void PsConnectionTrace::finish", 1
)[0]
require(
    "persistentEventLog" not in record_body
    and "diagnosticLog" not in record_body
    and "dropDiagnosticLog" not in record_body,
    "recording a connection stage must not perform SD-card logging",
)
require(
    "persistentEventLog" in trace_cpp
    and "kEntriesPerBatch" in trace_cpp
    and "elapsed_ms" in trace_cpp,
    "the completed trace must preserve stage timing in the release log",
)
for forbidden in ("access_token", "refresh_token", "regist_key", "morning"):
    require(
        forbidden not in trace_h and forbidden not in trace_cpp,
        f"connection trace must not retain sensitive field {forbidden}",
    )

require(
    "std::make_shared<PsConnectionTrace>" in controller
    and "connection_trace_" in controller
    and 'finish("ready", "first-video-rendered")' in controller,
    "the controller must correlate the whole launch and flush after first video",
)
require(
    'finish("failed"' in controller and 'finish("cancelled"' in controller,
    "failed and cancelled launches must flush their in-memory traces",
)
for stage in (
    "session-create",
    "control-offer",
    "session-start",
    "control-punch",
    "retry-decision",
):
    require(
        f'"{stage}"' in remote,
        f"remote diagnostics must identify the {stage} phase",
    )
require(
    "http_status" in remote and "elapsedMs" in remote,
    "remote diagnostics must retain HTTP status and phase duration",
)
for stage in ("chiaki-init", "chiaki-start", "data-hole", "connected", "quit"):
    require(
        f'"{stage}"' in session,
        f"session diagnostics must identify the {stage} phase",
    )
require(
    '"first-video-sample"' in bridge
    and '"audio-header"' in bridge
    and '"first-audio-packet"' in bridge,
    "the trace must distinguish transport success from missing media",
)
require(
    'persistentEventLog("ps-connect-preflight"' in ui
    and "auth_kind=" in ui
    and "worker-start" in ui
    and "planner" in ui,
    "failures before startStream must remain visible in release logs",
)
require(
    'persistentEventLog("ps-wakeup"' in manager
    and "discovery-init" in manager
    and "wakeup-send" in manager,
    "LAN wake failures must identify credential, discovery and send phases",
)

print("PS connection diagnostics regression checks passed")
