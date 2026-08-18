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
    cloud_model = read("src/ui/cloud_library_model.cpp")
    cloud_card = read("src/ui/cloud_game_card.cpp")
    grid_navigation = read("src/ui/grid_navigation.h")
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
    require("start += 4" in main_source and
            "cloud_visible_limit_ = 20" in main_header,
            "the cloud library must use a bounded four-column, 20-item grid")
    require("new CloudGameCard" in main_source and
            "PosterLoader::instance().load" in cloud_card,
            "cloud cards must own bounded poster loading")
    require('setImageFromRes("img/platform/xbox.png")' in cloud_card,
            "cloud cards must retain an Xbox placeholder when cover loading fails")
    require("image->setWidth(202)" in cloud_card and
            "image->setHeight(303)" in cloud_card and
            "ImageScalingType::FIT" in cloud_card,
            "portrait store covers must be shown at their full 2:3 aspect ratio")
    require('setImageFromRes("img/platform/xbox.png");\n    image->setFreeTexture(true)' not in cloud_card,
            "a card must not claim ownership of the shared Xbox placeholder texture")
    require("replacePosterTexture" in poster_source and
            poster_source.find("image->setFreeTexture(false)") <
            poster_source.find("image->setImageFromMem") <
            poster_source.find("image->setFreeTexture(true)"),
            "poster replacement must preserve the shared placeholder before owning the new texture")
    require("CloudLibraryFilter::Recent" in cloud_model and
            "CloudLibrarySort::RecentFirst" in cloud_model,
            "cloud filtering and sorting must remain in the testable model")
    for tab in ("cloud_all_tab_", "cloud_recent_tab_", "cloud_new_tab_"):
        require(tab in main_header + main_source,
                f"cloud category must be a directly focusable tab: {tab}")
    require("cloud_filter_ = CloudLibraryFilter::Recent" in main_header and
            main_source.find("add_cloud_tab(cloud_recent_tab_") <
            main_source.find("add_cloud_tab(cloud_new_tab_") <
            main_source.find("add_cloud_tab(cloud_all_tab_"),
            "cloud tabs must default to Recently played and order Recent, New, All")
    require("stepCloudTab(-1)" in main_source and
            "stepCloudTab(1)" in main_source,
            "ZL/ZR must switch directly between cloud category tabs")
    rebuild_body = method_body(
        main_source,
        "void MainActivity::rebuildCloudList()",
        "void MainActivity::refreshCloudTitles")
    require("console_list_->isChildFocused()" in rebuild_body and
            "giveFocus(selected_tab)" in rebuild_body,
            "rebuilding a cloud tab must hand focus off before deleting cards")
    require("giveFocus(cloud_search_btn_)" not in rebuild_body,
            "tab changes must not force focus back to Search")
    for button in ("BUTTON_Y", "BUTTON_LT", "BUTTON_RT", "BUTTON_X"):
        require(button in main_source,
                f"cloud library controller shortcut missing: {button}")
    for english_hint in ('"Back"', '"Search"', '"Previous tab"',
                         '"Next tab"', '"More / Refresh"'):
        require(english_hint not in main_source,
                f"bottom action hint must not be hard-coded in English: {english_hint}")
    require(main_source.count("}, true, false);") >= 4,
            "secondary cloud shortcuts must remain functional without crowding the hint bar")

    require("posterThumbnailUrl" in poster_source and
            "store-images.s-microsoft.com" in poster_source,
            "Microsoft Store covers must use a bounded thumbnail URL")
    for parameter in ("w=240", "h=360", "q=80", "format=jpg"):
        require(parameter in poster_source,
                f"poster thumbnail URL is missing {parameter}")
    require(poster_source.find("posterThumbnailUrl") <
            poster_source.find("http.get("),
            "poster URL must be resized before the HTTP request")
    require("kPosterMemoryCacheLimit = 8 * 1024 * 1024" in poster_source and
            "findCachedPosterLocked" in poster_source and
            "cachePosterLocked" in poster_source,
            "poster loading must reuse a bounded in-memory cache")
    require("g_queue.size() >= 32" not in poster_source,
            "loading more than 32 games must not discard leading poster jobs")
    require("void runQueueWorker()" in poster_source and
            "while (true)" in poster_source and
            "finishAndContinue" not in poster_source,
            "one persistent worker must drain a poster batch without racing detached thread teardown")
    require(poster_source.count('startNetworkWorker(\n        "poster-load"') == 1,
            "poster loading must not create one detached worker per cover")
    require("kMaximumPosterBytes = 1024 * 1024" in poster_source,
            "resized covers may use up to one MiB without accepting full-size images")
    require("kPosterDiskCacheLimit = 128 * 1024 * 1024" in poster_source and
            "readDiskPoster" in poster_source and
            "writeDiskPoster" in poster_source and
            'temporary = path + ".tmp"' in poster_source,
            "poster loading must use a bounded, atomic SD-card cache")
    require("cloud_page_start_ += 20" in main_source and
            "std::vector<api::CloudTitle> page_items" in main_source and
            "appendCloudCards(page_items, 0)" in main_source,
            "pagination must keep only one 20-card page mounted at a time")
    require("setContentOffsetY(0, false)" in main_source and
            "attachCloudPreviousButton" not in main_source,
            "forward pagination must reset stale scroll offset without a top previous-page control")
    require("CloudPageSentinel" in main_source and
            "loadPreviousCloudTitles" in main_source and
            "attachCloudPreviousSentinel" in main_source,
            "the bounded cloud window must navigate continuously in both directions")
    require("wireVerticalGridNavigation" in grid_navigation and
            "setCustomNavigationRoute" in grid_navigation and
            "rewireCloudNavigation" in main_source,
            "cloud rows must use explicit vertical focus routes")
    require("cloud_navigation_rows_.push_back" in main_source,
            "every appended card row must participate in focus routing")
    require('brls::getStr("lunarnx/main/cloud_grid_hint")' not in main_source and
            '"lunarnx/main/sort_by"' in main_source,
            "the grid must avoid duplicate controller hints and distinguish sorting from tabs")
    require("makeSectionHeader(" not in rebuild_body,
            "the selected cloud tab must not be repeated as a section heading")

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
    require("poster_batch" in main_source and
            "PosterLoader::BatchId poster_batch" in cloud_card,
            "cloud cards must retain the originating poster batch")

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
