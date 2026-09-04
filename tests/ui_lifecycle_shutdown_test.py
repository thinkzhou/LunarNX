#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text()
ACTIVITY = (ROOT / "src/ui/main_activity.cpp").read_text()
POSTER = (ROOT / "src/ui/poster_loader.cpp").read_text()
CONTROLLER = (ROOT / "src/app/stream_controller.cpp").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require(
    "getExitEvent()->subscribe" in MAIN
    and "PosterLoader::instance().shutdown()" in MAIN
    and "shutdownNetworkWorkers()" in MAIN,
    "application exit must stop poster work and drain network workers before Borealis destroys views",
)

require(
    "g_shutting_down" in POSTER
    and "g_worker_idle" in POSTER
    and "condition.wait" in POSTER
    and "networkWorkersShuttingDown" in POSTER,
    "poster shutdown must cancel in-flight HTTP/UI waits and wait for its worker to release raw views",
)

require(
    "void MainActivity::prepareConsoleListForReplacement" in ACTIVITY
    and ACTIVITY.count("prepareConsoleListForReplacement(") >= 5,
    "every dynamic Xbox/cloud list replacement must move focus away and clear stale routes",
)

require(
    "CloudPageSentinel([this, alive]" in ACTIVITY
    and "openForText(\n        [this, alive]" in ACTIVITY,
    "deferred paging and IME callbacks must carry the activity lifetime token",
)

require(
    "writeCacheAtomically" in CONTROLLER
    and "kMaxCloudLibraryCacheBytes" in CONTROLLER
    and "kMaxConsoleCacheBytes" in CONTROLLER,
    "Xbox caches must be size-bounded and atomically replaced so interrupted exits retain a valid file",
)

print("UI lifecycle shutdown test passed")
