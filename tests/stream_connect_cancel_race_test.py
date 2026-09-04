#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
XBOX_SOURCE = (ROOT / "src/ui/stream_loading_activity.cpp").read_text()
XBOX_HEADER = (ROOT / "src/ui/stream_loading_activity.h").read_text()
PS_SOURCE = (ROOT / "src/ui/ps_activity.cpp").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require(
    "CancelContext" in XBOX_HEADER and "ConnectionCancelState" in XBOX_SOURCE,
    "Xbox loading must share one atomic connection-cancellation state",
)

xbox_cancel = XBOX_SOURCE.split(
    "void StreamLoadingActivity::requestCancel()", 1
)[1].split("void StreamLoadingActivity::handleConnectionResult", 1)[0]
require(
    "ctrl_->requestStop();" in xbox_cancel,
    "the UI thread must only signal connection cancellation",
)
require(
    "stopStream(" not in xbox_cancel and "startNetworkWorker(" not in xbox_cancel,
    "a B press must not create a competing stop owner",
)
require(
    "scheduleCancelCleanup" in xbox_cancel,
    "a repeated B press must safely retry an unclaimed completed cleanup",
)

cleanup_body = XBOX_SOURCE.split(
    "void StreamLoadingActivity::runCancelCleanup", 1
)[1].split("bool StreamLoadingActivity::scheduleCancelCleanup", 1)[0]
require(
    cleanup_body.count("ctrl->stopStream(false);") == 1,
    "Xbox connection cancellation must have exactly one teardown call site",
)
require(
    "stop-cancelled-stream" not in XBOX_SOURCE,
    "the connection result must not schedule a second competing stop worker",
)
require(
    "markWorkerDone()" in XBOX_SOURCE
    and "tryClaimCleanup()" in XBOX_SOURCE,
    "the connection worker must hand teardown to a single claimed owner",
)

ps_cancel = PS_SOURCE.split("void cancel()", 1)[1].split(
    "void openStream()", 1
)[0]
repeat_start = ps_cancel.index("if (context_->cancel_requested.load())")
first_cancel = ps_cancel.index("context_->cancel_requested = true")
repeat_path = ps_cancel[repeat_start:first_cancel]
require(
    "schedulePsConnectCleanup" in repeat_path and "return;" in repeat_path,
    "PS repeated B must only retry the single claimed cleanup",
)
require(
    "popActivity" not in repeat_path
    and "requestCancel()" not in repeat_path
    and "stopStream(" not in repeat_path,
    "PS repeated B must not cancel, stop, or navigate a second time",
)

print("stream connect cancel race tests passed")
