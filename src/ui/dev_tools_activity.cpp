#ifdef __SWITCH__
#include "dev_tools_activity.h"

#include "ui_style.h"
#include "grid_navigation.h"
#include "../app/runtime_context.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"

#include <switch.h>
#include <algorithm>
#include <chrono>
#include <string>

namespace {

std::string formatDownloadRate(long long bytes_per_second) {
    if (bytes_per_second <= 0) return "--";
    if (bytes_per_second >= 1024 * 1024) {
        return std::to_string(bytes_per_second / (1024 * 1024)) + " MiB/s";
    }
    return std::to_string(std::max(1LL, bytes_per_second / 1024)) + " KiB/s";
}

std::string formatDownloadEta(long long remaining_bytes, long long bytes_per_second) {
    if (remaining_bytes <= 0) return "";
    if (bytes_per_second <= 0) return "ETA --";
    const long long seconds = (remaining_bytes + bytes_per_second - 1) / bytes_per_second;
    if (seconds >= 60) {
        return "ETA " + std::to_string(seconds / 60) + "m " +
            std::to_string(seconds % 60) + "s";
    }
    return "ETA " + std::to_string(seconds) + "s";
}

} // namespace

namespace lunar::ui {

DevToolsActivity::~DevToolsActivity() {
    alive_->store(false);
}

brls::View* DevToolsActivity::createContentView() {
    const auto& p = uiPalette();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->registerAction(brls::getStr("lunarnx/common/back"),
        brls::ControllerButton::BUTTON_B, [](brls::View*) -> bool {
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(18, 52, 32, 52);
    root->setBackgroundColor(p.background);

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setHeight(64);
    header->setAlignItems(brls::AlignItems::CENTER);
    auto* subtitle = makeMutedLabel(brls::getStr("lunarnx/dev/subtitle"), 13);
    subtitle->setGrow(1.0f);
    subtitle->setSingleLine(true);
    header->addView(subtitle);

    refresh_ = new brls::Button();
    refresh_->setText(brls::getStr("lunarnx/dev/refresh"));
    styleSecondaryButton(refresh_);
    refresh_->registerClickAction([this](brls::View*) -> bool {
        refreshVersions();
        return true;
    });
    header->addView(refresh_);

    upload_ = new brls::Button();
    upload_->setText(brls::getStr("lunarnx/dev/upload_log"));
    upload_->setMarginLeft(10);
    stylePrimaryButton(upload_);
    upload_->registerClickAction([this](brls::View*) -> bool {
        uploadLog();
        return true;
    });
    header->addView(upload_);
    root->addView(header);

    status_ = makeMutedLabel(brls::getStr("lunarnx/dev/loading"), 13);
    status_->setHeight(34);
    status_->setSingleLine(true);
    root->addView(status_);
    versions_ = new brls::Box(brls::Axis::COLUMN);
    root->addView(versions_);
    scroll->setContentView(root);

    refreshVersions();
    return makeAppFrame(brls::getStr("lunarnx/dev/title"), scroll);
}

void DevToolsActivity::setBusy(bool busy) {
    busy_.store(busy);
    const auto state = busy ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED;
    if (refresh_) refresh_->setState(state);
    if (upload_) upload_->setState(state);
}

void DevToolsActivity::refreshVersions() {
    if (busy_.exchange(true)) return;
    setBusy(true);
    if (status_) status_->setText(brls::getStr("lunarnx/dev/loading"));
    auto alive = alive_;
    if (!platform::startNetworkWorker("dev-versions", [this, alive]() {
        app::DevBridgeClient client;
        std::vector<app::DevBuild> builds;
        std::string error;
        const bool ok = client.fetchVersions(builds, error);
        brls::sync([this, alive, ok, builds = std::move(builds), error = std::move(error)]() {
            if (!alive->load()) return;
            setBusy(false);
            if (!ok) {
                if (status_) status_->setText(brls::getStr("lunarnx/dev/load_failed") + ": " + error);
                return;
            }
            renderVersions(builds);
        });
    })) {
        setBusy(false);
        if (status_) status_->setText(brls::getStr("lunarnx/dev/worker_failed"));
    }
}

void DevToolsActivity::renderVersions(const std::vector<app::DevBuild>& builds) {
    if (!versions_) return;
    if (versions_->isChildFocused() && refresh_) {
        brls::Application::giveFocus(refresh_);
    }
    versions_->clearViews();
    if (status_) status_->setText(brls::getStr("lunarnx/dev/available") + ": " +
        std::to_string(builds.size()));
    const auto& p = uiPalette();
    std::vector<brls::View*> install_buttons;
    for (const auto& build : builds) {
        auto* card = makeUiCard(brls::Axis::ROW);
        card->setHeight(92);
        card->setPadding(10, 18, 10, 18);
        card->setMarginBottom(10);
        card->setAlignItems(brls::AlignItems::CENTER);
        auto* copy = new brls::Box(brls::Axis::COLUMN);
        copy->setGrow(1.0f);
        auto* version = new brls::Label();
        version->setText(build.version);
        version->setFontSize(19);
        version->setTextColor(p.text);
        copy->addView(version);
        auto* notes = makeMutedLabel(build.notes, 13);
        notes->setSingleLine(true);
        copy->addView(notes);
        copy->addView(makeMutedLabel(build.published_at + "  " +
            std::to_string(build.size / (1024 * 1024)) + " MiB  " +
            build.git_commit, 11));
        card->addView(copy);
        auto* install = new brls::Button();
        install->setWidth(170);
        install->setText(brls::getStr("lunarnx/dev/install"));
        stylePrimaryButton(install);
        install->registerClickAction([this, build](brls::View*) -> bool {
            confirmInstall(build);
            return true;
        });
        card->addView(install);
        versions_->addView(card);
        install_buttons.push_back(install);
    }

    std::vector<std::vector<brls::View*>> navigation_rows{{refresh_, upload_}};
    for (auto* install : install_buttons) navigation_rows.push_back({install});
    wireVerticalGridNavigation(navigation_rows);
}

void DevToolsActivity::confirmInstall(app::DevBuild build) {
    auto* dialog = new brls::Dialog(
        brls::getStr("lunarnx/dev/install_confirm") + "\n\n" +
        build.version + "\n" + build.notes);
    dialog->addButton(brls::getStr("lunarnx/common/cancel"), []() {});
    dialog->addButton(brls::getStr("lunarnx/dev/install"),
        [this, build = std::move(build)]() mutable { installBuild(std::move(build)); });
    dialog->open();
}

void DevToolsActivity::installBuild(app::DevBuild build) {
    if (busy_.exchange(true)) return;
    setBusy(true);
    if (status_) status_->setText(brls::getStr("lunarnx/dev/installing") + " " + build.version);
    showDownloadProgress(build.version);
    auto alive = alive_;
    const std::string target = app::runningNroPath();
    if (!platform::startNetworkWorker("dev-install", [this, alive, build = std::move(build), target]() {
        app::DevBridgeClient client;
        std::string error;
        const long long manifest_size = !build.compressed_download_url.empty() &&
            build.compressed_size > 0 ? build.compressed_size : build.size;
        const std::string version = build.version;
        const auto progress_started = std::chrono::steady_clock::now();
        auto progress_last_sample = progress_started;
        long long progress_last_bytes = 0;
        long long progress_speed_bps = 0;
        const bool ok = client.install(build, target, error,
            [this, alive, manifest_size, version, progress_started,
             progress_last_sample, progress_last_bytes, progress_speed_bps]
            (long long downloaded, long long total) mutable {
                const long long expected = total > 0 ? total : manifest_size;
                const int percent = expected > 0
                    ? static_cast<int>((downloaded * 100) / expected) : 0;
                const auto now = std::chrono::steady_clock::now();
                const auto sample_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - progress_last_sample).count();
                if (sample_ms >= 200 && downloaded >= progress_last_bytes) {
                    const long long sample_speed =
                        (downloaded - progress_last_bytes) * 1000 / sample_ms;
                    progress_speed_bps = progress_speed_bps == 0 ? sample_speed
                        : (progress_speed_bps * 3 + sample_speed) / 4;
                    progress_last_sample = now;
                    progress_last_bytes = downloaded;
                } else if (progress_speed_bps == 0) {
                    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - progress_started).count();
                    if (elapsed_ms >= 500 && downloaded > 0) {
                        progress_speed_bps = downloaded * 1000 / elapsed_ms;
                    }
                }
                const long long speed_bps = progress_speed_bps;
                brls::sync([this, alive, downloaded, expected, percent, speed_bps, version]() {
                    if (!alive->load()) return;
                    updateDownloadProgress(downloaded, expected, percent, speed_bps, version);
                });
            });
        brls::sync([this, alive, ok, error = std::move(error), target]() {
            if (!alive->load()) return;
            if (!ok) {
                closeDownloadProgress();
                setBusy(false);
                if (status_) status_->setText(brls::getStr("lunarnx/dev/install_failed") + ": " + error);
                return;
            }
            updateDownloadProgress(1, 1, 100, 0, brls::getStr("lunarnx/dev/restarting"));
            if (status_) status_->setText(brls::getStr("lunarnx/dev/restarting"));
            envSetNextLoad(target.c_str(), target.c_str());
            brls::Application::quit();
        });
    })) {
        closeDownloadProgress();
        setBusy(false);
        if (status_) status_->setText(brls::getStr("lunarnx/dev/worker_failed"));
    }
}

void DevToolsActivity::showDownloadProgress(const std::string& version) {
    if (progress_dialog_) return;
    const auto& p = uiPalette();
    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setWidth(520);
    content->setPadding(30, 34, 30, 34);
    content->setAlignItems(brls::AlignItems::CENTER);

    auto* spinner = new brls::ProgressSpinner(brls::ProgressSpinnerSize::LARGE);
    spinner->setMarginBottom(18);
    content->addView(spinner);

    auto* title = new brls::Label();
    title->setText(brls::getStr("lunarnx/dev/installing") + " " + version);
    title->setFontSize(20);
    title->setTextColor(p.text);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMarginBottom(14);
    content->addView(title);

    auto* track = new brls::Box();
    track->setWidth(440);
    track->setHeight(12);
    track->setBackgroundColor(p.surface_alt);
    track->setAlignItems(brls::AlignItems::FLEX_START);
    progress_fill_ = new brls::Box();
    progress_fill_->setWidth(2);
    progress_fill_->setHeight(12);
    progress_fill_->setBackgroundColor(p.accent);
    track->addView(progress_fill_);
    content->addView(track);

    progress_label_ = makeMutedLabel("0%", 14);
    progress_label_->setMarginTop(12);
    progress_label_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    content->addView(progress_label_);

    progress_dialog_ = new brls::Dialog(content);
    progress_dialog_->setCancelable(false);
    progress_dialog_->open();
}

void DevToolsActivity::updateDownloadProgress(long long downloaded, long long expected,
                                              int percent, long long speed_bps,
                                              const std::string& version) {
    const int safe_percent = std::clamp(percent, 0, 100);
    if (progress_fill_) progress_fill_->setWidth(std::max(2, safe_percent * 440 / 100));
    std::string detail = std::to_string(safe_percent) + "% (" +
        std::to_string(downloaded / (1024 * 1024)) + "/" +
        std::to_string(expected / (1024 * 1024)) + " MiB)  " +
        formatDownloadRate(speed_bps);
    if (expected > downloaded) {
        detail += "  " + formatDownloadEta(expected - downloaded, speed_bps);
    }
    if (progress_label_) progress_label_->setText(detail);
    if (status_) status_->setText(brls::getStr("lunarnx/dev/installing") + " " +
        version + " — " + detail);
}

void DevToolsActivity::closeDownloadProgress() {
    if (progress_dialog_) progress_dialog_->close();
    progress_dialog_ = nullptr;
    progress_label_ = nullptr;
    progress_fill_ = nullptr;
}

void DevToolsActivity::uploadLog() {
    if (busy_.exchange(true)) return;
    setBusy(true);
    if (status_) status_->setText(brls::getStr("lunarnx/dev/uploading_log"));
    auto alive = alive_;
    if (!platform::startNetworkWorker("dev-log-upload", [this, alive]() {
        app::DevBridgeClient client;
        std::string error;
        std::string log_id;
        const bool ok = client.uploadLog(get_diagnostic_log_path(), log_id, error);
        brls::sync([this, alive, ok, log_id = std::move(log_id), error = std::move(error)]() {
            if (!alive->load()) return;
            setBusy(false);
            if (status_) status_->setText(ok ? brls::getStr("lunarnx/dev/upload_done") + ": " + log_id
                : brls::getStr("lunarnx/dev/upload_failed") + ": " + error);
        });
    })) {
        setBusy(false);
        if (status_) status_->setText(brls::getStr("lunarnx/dev/worker_failed"));
    }
}

} // namespace lunar::ui
#endif
