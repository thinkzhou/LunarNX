#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/ui/stream_view.cpp").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


start = SOURCE.index("void StreamView::stopAndReturn()")
end = SOURCE.index("void StreamView::setQuickMenuVisible", start)
body = SOURCE[start:end]
block = body.find("brls::Application::blockInputs")
worker = body.find('startNetworkWorker(\n        "stop-stream"')
stop = body.find("runtime->stopStream", worker)
sync = body.find("brls::sync", stop)
unblock = body.find("brls::Application::unblockInputs", sync)
pop = body.find("brls::Application::popActivity")

require(
    min(block, worker, stop, sync, unblock, pop) >= 0,
    "StreamView stop navigation lifecycle is incomplete",
)
require(
    block < worker < stop < sync < unblock < pop,
    "StreamView must return only after background stream cleanup completes",
)
require(
    "if (!alive->load()) return;" in body[unblock:pop],
    "StreamView completion must not navigate after the activity is destroyed",
)
require(
    body.count("brls::Application::unblockInputs") == 2 and
    "if (!started)" in body[unblock:],
    "StreamView must restore input when the stop worker cannot start",
)

print("stream exit navigation test passed")
