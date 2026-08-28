#!/usr/bin/env python3
from pathlib import Path
import json


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/ui/stream_view.cpp").read_text()
HEADER = (ROOT / "src/ui/stream_view.h").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


start = SOURCE.index("void StreamView::stopAndReturn()")
end = SOURCE.index("void StreamView::setQuickMenuVisible", start)
body = SOURCE[start:end]
show_overlay = body.find("stopping_overlay_->setVisibility")
block = body.find("brls::Application::blockInputs")
worker = body.find('startNetworkWorker(\n        "stop-stream"')
stop = body.find("runtime->stopStream", worker)
sync = body.find("brls::sync", stop)
unblock = body.find("brls::Application::unblockInputs", sync)
pop = body.find("brls::Application::popActivity")

require(
    min(show_overlay, block, worker, stop, sync, unblock, pop) >= 0,
    "StreamView stop navigation lifecycle is incomplete",
)
require(
    show_overlay < block < worker < stop < sync < unblock < pop,
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
require(
    "brls::Box* stopping_overlay_" in HEADER and
    "new brls::ProgressSpinner" in SOURCE and
    'brls::getStr("lunarnx/stream/stopping")' in SOURCE,
    "StreamView must show a localized progress overlay while stopping",
)
failure = body[body.index("if (!started)"):]
require(
    "stopping_overlay_->setVisibility" in failure and
    "brls::Visibility::GONE" in failure,
    "StreamView must hide the progress overlay if the stop worker cannot start",
)

for locale in ("en-US", "zh-Hans", "zh-Hant"):
    translations = json.loads(
        (ROOT / "romfs/i18n" / locale / "lunarnx.json").read_text()
    )
    require(
        translations["stream"].get("stopping"),
        f"{locale} must translate the stopping progress message",
    )

print("stream exit navigation test passed")
