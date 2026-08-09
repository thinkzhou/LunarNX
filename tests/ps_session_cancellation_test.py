#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SESSION = (ROOT / "src/ps/ps_stream_session.cpp").read_text()
CONTROLLER = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
ACTIVITY = (ROOT / "src/tools/ps_mock_lifecycle_activity.cpp").read_text()


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


for stage in ("before init", "after configure", "after init", "after start"):
    require(f'startupCancelled("{stage}")' in SESSION,
            f"real Chiaki startup must observe cancellation {stage}")

require("releaseRemoteHolepunch()" in SESSION and
        "chiaki_session_fini(&session_)" in SESSION and
        "stop();" in SESSION,
        "each cancellation stage must release the resources it already owns")
require("if (cancel_requested_.load())" in CONTROLLER and
        "if (mock_session_) mock_session_->stop();" in CONTROLLER,
        "controller must re-check cancellation after session startup")
after_connector = CONTROLLER.split("connector->connect", 1)[1].split(
    "// Create long-lived components", 1)[0]
require("void PsStreamController::releasePendingRemoteResult()" in CONTROLLER and
        "chiaki_holepunch_session_fini(remote_result_.holepunch_session)" in CONTROLLER and
        "releasePendingRemoteResult();" in after_connector,
        "cancellation after connector handoff must release the pending holepunch session")

request_cancel = CONTROLLER.split(
    "void PsStreamController::requestCancel()", 1)[1].split(
        "void PsStreamController::stopStream", 1)[0]
require("mock_session_" not in request_cancel,
        "lock-free cancellation must not race an unprotected mock-session pointer")
require("std::thread cancel_thread" in ACTIVITY and
        "cancel_race=ok" in ACTIVITY,
        "Switch mock lifecycle probe must execute a start/cancel/stop race")

print("PS session cancellation test passed")
