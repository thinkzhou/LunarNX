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
worker = body.index('startNetworkWorker(\n        "stop-stream"')
pop = body.index("brls::Application::popActivity")

require(
    worker < pop and "brls::sync" not in body,
    "StreamView navigation must not wait for background stream cleanup",
)

print("stream exit navigation test passed")
