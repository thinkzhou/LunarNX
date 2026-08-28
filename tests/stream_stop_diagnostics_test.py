#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = (ROOT / "src/app/stream_controller.cpp").read_text()
SESSION = (ROOT / "src/app/xbox_stream_session.cpp").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


controller_start = CONTROLLER.index("void StreamController::stopStream(bool")
controller_end = CONTROLLER.index("bool StreamController::resumeAfterForeground", controller_start)
controller_body = CONTROLLER[controller_start:controller_end]
require(
    "persistentEventLog" in controller_body and
    "phase=operation-lock begin" in controller_body and
    "phase=operation-lock done wait_ms=%lld slow=%s" in controller_body and
    "stop stream complete total_ms=%lld slow=%s" in controller_body,
    "StreamController must report lock wait and total stop duration",
)

stop_start = SESSION.index("void XboxStreamSession::stop(bool")
stop_end = SESSION.index("std::string XboxStreamSession::sessionId", stop_start)
stop_body = SESSION[stop_start:stop_end]
require(
    "persistentEventLog" in stop_body and
    "phase=worker-join begin" in stop_body and
    "phase=worker-join done elapsed_ms=%lld slow=%s" in stop_body and
    "stop complete total_ms=%lld slow=%s" in stop_body,
    "Xbox stop must report worker-join and total duration",
)

cleanup_start = SESSION.index("void XboxStreamSession::cleanupResources(bool")
cleanup_end = SESSION.index("bool XboxStreamSession::failStart", cleanup_start)
cleanup_body = SESSION[cleanup_start:cleanup_end]
require(
    "persistentEventLog" in cleanup_body,
    "Stop timing must remain available when release diagnostics are disabled",
)
for phase in (
    "channels",
    "session-delete",
    "transport",
    "media",
    "input",
    "diagnostics-writer",
):
    require(
        f"phase={phase} begin" in cleanup_body and
        f"phase={phase} done elapsed_ms=%lld slow=%s" in cleanup_body,
        f"Xbox cleanup must report the {phase} phase duration",
    )
require(
    "cleanup complete total_ms=%lld slow=%s" in cleanup_body,
    "Xbox cleanup must report its total duration",
)

print("stream stop diagnostics test passed")
