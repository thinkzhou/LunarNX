#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def method_body(source, signature, next_signature):
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def require_poster_batch_cancelled_before_launch(body, label):
    state_guard = body.find("ctrl_->getState()")
    cancel = body.find("PosterLoader::instance().beginBatch()")
    push = body.find("brls::Application::pushActivity")

    require(state_guard >= 0, f"{label} must keep its stream-state guard")
    require(cancel >= 0, f"{label} must invalidate poster work before launch")
    require(push >= 0, f"{label} must launch through a loading activity")
    require(state_guard < cancel < push,
            f"{label} must invalidate posters after guards and before launch")


def main():
    source = Path("src/ui/main_activity.cpp").read_text()

    console = method_body(
        source,
        "void MainActivity::startConsoleStream",
        "void MainActivity::refreshConsoles")
    cloud = method_body(
        source,
        "void MainActivity::startCloudTitleStream",
        "} // namespace lunar::ui")

    require_poster_batch_cancelled_before_launch(console, "console stream")
    require_poster_batch_cancelled_before_launch(cloud, "cloud stream")

    print("Stream poster lifecycle tests passed")


if __name__ == "__main__":
    main()
