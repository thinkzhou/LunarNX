#ifdef __SWITCH__
#include "dev_tools_activity.h"

#include "ui_style.h"
#include "../app/runtime_context.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"

#include <switch.h>

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
    versions_->clearViews();
    if (status_) status_->setText(brls::getStr("lunarnx/dev/available") + ": " +
        std::to_string(builds.size()));
    const auto& p = uiPalette();
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
    }
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
    auto alive = alive_;
    const std::string target = app::runningNroPath();
    if (!platform::startNetworkWorker("dev-install", [this, alive, build = std::move(build), target]() {
        app::DevBridgeClient client;
        std::string error;
        const long long manifest_size = build.size;
        const std::string version = build.version;
        const bool ok = client.install(build, target, error,
            [this, alive, manifest_size, version](long long downloaded, long long total) {
                const long long expected = total > 0 ? total : manifest_size;
                const int percent = expected > 0
                    ? static_cast<int>((downloaded * 100) / expected) : 0;
                brls::sync([this, alive, downloaded, expected, percent, version]() {
                    if (!alive->load() || !status_) return;
                    status_->setText(brls::getStr("lunarnx/dev/installing") + " " + version +
                        " — " + std::to_string(percent) + "% (" +
                        std::to_string(downloaded / (1024 * 1024)) + "/" +
                        std::to_string(expected / (1024 * 1024)) + " MiB)");
                });
            });
        brls::sync([this, alive, ok, error = std::move(error), target]() {
            if (!alive->load()) return;
            if (!ok) {
                setBusy(false);
                if (status_) status_->setText(brls::getStr("lunarnx/dev/install_failed") + ": " + error);
                return;
            }
            if (status_) status_->setText(brls::getStr("lunarnx/dev/restarting"));
            envSetNextLoad(target.c_str(), target.c_str());
            brls::Application::quit();
        });
    })) {
        setBusy(false);
        if (status_) status_->setText(brls::getStr("lunarnx/dev/worker_failed"));
    }
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
