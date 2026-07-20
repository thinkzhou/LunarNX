#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def read(path: str) -> str:
    file_path = ROOT / path
    require(file_path.exists(), f"missing {path}")
    return file_path.read_text()


def method_body(source: str, signature: str, next_signature: str) -> str:
    start = source.find(signature)
    require(start >= 0, f"missing method: {signature}")
    end = source.find(next_signature, start)
    require(end > start, f"missing method boundary after: {signature}")
    return source[start:end]


def main() -> None:
    loading_header = read("src/ui/stream_loading_activity.h")
    loading_source = read("src/ui/stream_loading_activity.cpp")
    main_header = read("src/ui/main_activity.h")
    main_source = read("src/ui/main_activity.cpp")
    controller_header = read("src/app/stream_controller.h")
    controller_source = read("src/app/stream_controller.cpp")
    api_source = read("src/api/xbox_api_client.cpp")
    poster_header = read("src/ui/poster_loader.h")
    poster_source = read("src/ui/poster_loader.cpp")

    require("acknowledgeFailure" in loading_header + loading_source,
            "stream startup failures must wait for explicit acknowledgement")
    require("failure_pending_" in loading_header and
            "error_card_" in loading_header,
            "the loading activity must retain a visible failure card")
    failure_body = method_body(
        loading_source,
        "void StreamLoadingActivity::handleConnectionResult",
        "void StreamLoadingActivity::showFailure")
    require("finish(StreamLaunchResult::Failed" not in failure_body and
            "returnToMain();" not in failure_body,
            "a failed connection must not immediately return to MainActivity")
    require("ControllerButton::BUTTON_X" in loading_source and
            "ControllerButton::BUTTON_Y" in loading_source,
            "common controller buttons must acknowledge the failure prompt")

    require("setListLoading" in main_header + main_source and
            "ProgressSpinner" in main_source,
            "Xbox and xCloud fetches must show an inline loading card")
    require("setFetchControlsEnabled" in main_header + main_source and
            "ButtonState::DISABLED" in main_source,
            "fetch controls must be disabled while a request is running")
    require("used_cached_fallback" in main_source,
            "live refresh fallback must be reported as cached data")
    require("restoreCloudTitlesFromCache" in controller_header + controller_source,
            "the controller must expose a cache-only cloud library restore")
    cloud_source_body = method_body(
        main_source,
        "void MainActivity::setStreamSource",
        "void MainActivity::highlightStreamSource")
    require("restoreCloudTitlesFromCache" in cloud_source_body,
            "entering xCloud must restore the disk cache without networking")
    require("fetchCloudTitles(true)" in main_source,
            "Refresh Library must remain an explicit live refresh")

    recent_index = api_source.find("recent_raw = getRecentCloudTitles")
    new_index = api_source.find("auto new_ids = getNewCloudProductIds")
    hydrate_index = api_source.find("hydrateCatalogProducts(hydrate_ids")
    require(0 <= recent_index < hydrate_index and 0 <= new_index < hydrate_index,
            "recent and new title IDs must be known before catalog hydration")
    require("kMaxCatalogHydrate = 80" in api_source,
            "one catalog request should cover recent, new, and first-screen titles")
    require("titles, 12, 4" in main_source,
            "the All Games first row must request poster images")

    require("posterThumbnailUrl" in poster_source and
            "store-images.s-microsoft.com" in poster_source,
            "Microsoft Store covers must use a bounded thumbnail URL")
    for parameter in ("w=240", "h=360", "q=80", "format=jpg"):
        require(parameter in poster_source,
                f"poster thumbnail URL is missing {parameter}")
    require(poster_source.find("posterThumbnailUrl") <
            poster_source.find("http.get("),
            "poster URL must be resized before the HTTP request")

    require("using BatchId = uint32_t" in poster_header and
            "beginBatch" in poster_header,
            "poster loading must expose a generation-scoped batch API")
    begin_batch_body = method_body(
        poster_source,
        "PosterLoader::BatchId PosterLoader::beginBatch()",
        "void PosterLoader::load")
    require("g_queue" in begin_batch_body and
            "ptrUnlock" in begin_batch_body,
            "starting a new poster batch must unlock and cancel queued views")
    load_body = method_body(
        poster_source,
        "void PosterLoader::load",
        "void PosterLoader::clear")
    batch_guard = load_body.find("batch != g_batch")
    pointer_lock = load_body.find("view->ptrLock()")
    queue_insert = load_body.find("g_queue.push_back")
    require(0 <= batch_guard < pointer_lock < queue_insert,
            "poster views must be generation-checked and locked before queueing")
    require("isBatchCurrent" in poster_source and
            poster_source.find("isBatchCurrent") <
            poster_source.find("image->setImageFromMem"),
            "stale poster callbacks must not write to replaced image views")
    require("[alive, pending, poster_batch]" in main_source and
            "load(poster.image, poster.url, poster_batch)" in main_source,
            "delayed poster enqueue callbacks must retain the originating batch")

    main_destructor = method_body(
        main_source,
        "MainActivity::~MainActivity()",
        "brls::View* MainActivity::createContentView")
    require("PosterLoader::instance().beginBatch()" in main_destructor,
            "destroying MainActivity must invalidate queued poster callbacks")

    for signature, next_signature in (
        ("void MainActivity::refreshConsoles()",
         "void MainActivity::setConsoleListMessage"),
        ("void MainActivity::setConsoleListMessage",
         "void MainActivity::setListLoading"),
        ("void MainActivity::setListLoading",
         "void MainActivity::setFetchControlsEnabled"),
        ("void MainActivity::rebuildCloudList()",
         "void MainActivity::refreshCloudTitles"),
    ):
        body = method_body(main_source, signature, next_signature)
        require(body.find("PosterLoader::instance().beginBatch()") <
                body.find("console_list_->clearViews()"),
                f"{signature} must invalidate poster jobs before clearing views")

    print("xCloud library UX tests passed")


if __name__ == "__main__":
    main()
