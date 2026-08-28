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
show_overlay = body.find("showStoppingOverlay()")
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
    "void showStoppingOverlay();" in HEADER and
    "new brls::ProgressSpinner" in SOURCE and
    'brls::getStr("lunarnx/stream/stopping")' in SOURCE,
    "StreamView must show a localized progress overlay while stopping",
)
create_start = SOURCE.index("brls::View* StreamView::createContentView()")
create_end = SOURCE.index("void StreamView::showStoppingOverlay()", create_start)
create_body = SOURCE[create_start:create_end]
show_end = SOURCE.index("void StreamView::stopAndReturn()", create_end)
show_body = SOURCE[create_end:show_end]
require(
    "new brls::ProgressSpinner" not in create_body and
    "new brls::ProgressSpinner" in show_body and
    "content_root_->addView(stopping_overlay_," in show_body and
    "content_root_->getChildren().size()" in show_body,
    "The stopping spinner must be created lazily after the user exits",
)
overlay_gone = show_body.find("brls::Visibility::GONE")
overlay_added = show_body.find("content_root_->addView(stopping_overlay_,")
overlay_visible = show_body.find("brls::Visibility::VISIBLE", overlay_added)
require(
    min(overlay_gone, overlay_added, overlay_visible) >= 0 and
    overlay_gone < overlay_added < overlay_visible,
    "The stopping overlay must be attached only on exit before it is shown",
)
failure = body[body.index("if (!started)"):]
require(
    "stopping_overlay_->setVisibility" in failure and
    "brls::Visibility::GONE" in failure,
    "StreamView must hide the progress overlay if the stop worker cannot start",
)
require(
    "stop_started_ = false" not in body and "running_ = true" not in body and
    "runtime->stopStream(report_disconnect);" in body,
    "a failed stop worker must finish terminal cleanup instead of reviving a cancelled stream",
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
