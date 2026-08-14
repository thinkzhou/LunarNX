#ifdef __SWITCH__
#include "main_activity.h"
#include "stream_loading_activity.h"
#include "stream_settings_activity.h"
#include "about_activity.h"
#include "ui_style.h"
#include "poster_loader.h"
#include "recycling_list.hpp"
#include "../diagnostics.h"
#include "../platform/network_worker.h"
#include <utility>
#include <vector>
#include <functional>
#include <algorithm>
#include <cctype>

namespace lunar::ui {

namespace {

std::string streamStateText(app::StreamState state, const std::string& info) {
    if (!info.empty()) return info;
    switch (state) {
        case app::StreamState::Authenticating:
            return brls::getStr("lunarnx/main/authenticating");
        case app::StreamState::Connecting:
            return brls::getStr("lunarnx/main/connecting");
        case app::StreamState::Streaming:
            return brls::getStr("lunarnx/common/streaming");
        case app::StreamState::Disconnected:
            return brls::getStr("lunarnx/common/disconnected");
        case app::StreamState::Error:
            return brls::getStr("lunarnx/common/connection_failed");
        case app::StreamState::Idle:
        default: return "";
    }
}

}

MainActivity::MainActivity(std::shared_ptr<app::StreamController> ctrl) : ctrl_(std::move(ctrl)) {
    const auto saved = loadStreamSettings();
    stream_width_ = saved.width;
    stream_height_ = saved.height;
    stream_bitrate_kbps_ = saved.bitrate_kbps;
    preferred_game_language_ = saved.preferred_game_language;
    video_backend_ = saved.video_backend;
    post_process_mode_ = saved.post_process_mode;
    dithering_enabled_ = saved.dithering_enabled;
    vibration_enabled_ = saved.vibration_enabled;
    rumble_strength_percent_ = saved.rumble_strength_percent;
    ctrl_->setDefaultVideoBackend(video_backend_);
    ctrl_->setRumbleEnabled(vibration_enabled_);
    ctrl_->setRumbleStrengthPercent(rumble_strength_percent_);
    ctrl_->setPreferredGameLanguage(preferred_game_language_);
    if (ctrl_->getForceRegionIp().empty()) {
        ctrl_->setForceRegionIp("4.2.2.2");
    }
}

MainActivity::~MainActivity() {
    alive_->store(false);
    PosterLoader::instance().beginBatch();
    if (cloud_list_) {
        cloud_list_->setDataSource(nullptr);
    }
    if (connecting_->load()) {
        auto ctrl = ctrl_;
        lunar::platform::startNetworkWorker("stop-stream", [ctrl]() {
            ctrl->stopStream(false);
        });
    }
}

brls::View* MainActivity::createContentView() {
    lunar::diagnosticLog("ui-main", "MainActivity create has_credentials=%s consoles=%zu",
                         ctrl_->hasCredentials() ? "true" : "false",
                         ctrl_->getConsoles().size());

    const auto& p = uiPalette();

    auto* workspace = new brls::Box(brls::Axis::ROW);
    workspace->setBackgroundColor(p.background);
    workspace->registerAction("Back",
        brls::ControllerButton::BUTTON_B,
        [](brls::View*) -> bool {
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    auto* sidebar = new brls::Box(brls::Axis::COLUMN);
    sidebar->setWidth(220);
    sidebar->setPadding(24, 18, 24, 18);
    sidebar->setBackgroundColor(p.surface);

    source_xbox_ = makeSidebarButton(
        brls::getStr("lunarnx/common/remote_play"), true, UiIcon::Console);
    source_xbox_->registerClickAction([this](brls::View*) -> bool {
        setStreamSource(StreamSource::Xbox);
        return true;
    });
    sidebar->addView(source_xbox_);
    source_cloud_ = makeSidebarButton(brls::getStr("lunarnx/common/xcloud"),
        false, UiIcon::Cloud);
    source_cloud_->registerClickAction([this](brls::View*) -> bool {
        setStreamSource(StreamSource::Cloud);
        return true;
    });
    sidebar->addView(source_cloud_);

    auto* settings_btn = makeSidebarButton(
        brls::getStr("lunarnx/common/xbox_settings"), false, UiIcon::Settings);
    settings_btn->registerClickAction([this](brls::View*) -> bool {
        openStreamSettings();
        return true;
    });
    sidebar->addView(settings_btn);
    auto* about_btn = makeSidebarButton(brls::getStr("lunarnx/common/about"),
        false, UiIcon::Info);
    about_btn->registerClickAction([](brls::View*) -> bool {
        brls::Application::pushActivity(
            new AboutActivity(), brls::TransitionAnimation::NONE);
        return true;
    });
    sidebar->addView(about_btn);
    auto* sign_out_btn = makeSidebarButton(brls::getStr("lunarnx/common/sign_out"),
        false, UiIcon::SignOut);
    sign_out_btn->registerClickAction([this](brls::View*) -> bool {
        confirmSignOut();
        return true;
    });
    sidebar->addView(sign_out_btn);

    auto* account = new brls::Box(brls::Axis::COLUMN);
    account->setGrow(1.0f);
    account->setJustifyContent(brls::JustifyContent::FLEX_END);
    account->addView(makeMutedLabel(brls::getStr("lunarnx/common/account"), 12));
    gamer_tag_ = new brls::Label();
    gamer_tag_->setFontSize(15);
    gamer_tag_->setTextColor(p.text);
    gamer_tag_->setSingleLine(true);
    account->addView(gamer_tag_);
    sidebar->addView(account);
    workspace->addView(sidebar);

    auto* scroll = new brls::ScrollingFrame();
    scroll_frame_ = scroll;
    scroll->setGrow(1.0f);
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(24, 42, 32, 42);
    root->setBackgroundColor(p.background);
    scroll->setContentView(root);
    workspace->addView(scroll);

    const bool can_use_console_api = ctrl_->hasCredentials();
    if (ctrl_->isMockMode()) {
        gamer_tag_->setText(brls::getStr("lunarnx/common/mock_mode"));
    } else if (can_use_console_api) {
        std::string gamertag = ctrl_->getGamertag();
        gamer_tag_->setText(gamertag.empty()
            ? brls::getStr("lunarnx/common/signed_in_microsoft")
            : gamertag);
    } else {
        gamer_tag_->setText(brls::getStr("lunarnx/common/not_signed_in"));
    }

    highlightStreamSource();

    auto* content_header = new brls::Box(brls::Axis::ROW);
    content_header->setHeight(68);
    content_header->setAlignItems(brls::AlignItems::CENTER);
    auto* content_labels = new brls::Box(brls::Axis::COLUMN);
    content_labels->setGrow(1.0f);
    content_title_ = new brls::Label();
    content_title_->setFontSize(25);
    content_title_->setTextColor(p.text);
    content_labels->addView(content_title_);
    content_subtitle_ = makeMutedLabel("", 13);
    content_labels->addView(content_subtitle_);
    content_header->addView(content_labels);

    auto* refreshBtn = new brls::Button();
    refresh_btn_ = refreshBtn;
    stylePrimaryButton(refreshBtn);
    updateRefreshButtonLabel();
    refreshBtn->registerClickAction([this](brls::View*) -> bool {
        refreshCurrentSource();
        return true;
    });
    content_header->addView(refreshBtn);

    auto* searchBtn = new brls::Button();
    cloud_search_btn_ = searchBtn;
    styleSecondaryButton(searchBtn);
    searchBtn->setMarginLeft(10);
    updateCloudSearchButtonLabel();
    searchBtn->registerClickAction([this](brls::View*) -> bool {
        if (stream_source_ != StreamSource::Cloud) {
            if (status_) {
                status_->setText(brls::getStr("lunarnx/main/switch_to_xcloud"));
            }
            return true;
        }
        promptCloudSearch();
        return true;
    });
    content_header->addView(searchBtn);
    root->addView(content_header);

    console_list_ = new brls::Box(brls::Axis::COLUMN);
    console_list_->setMarginBottom(18);
    root->addView(console_list_);

    // Cloud list reuses console_list_ plain box for emulator stability.
    // RecyclingList was crashing under Ryubing during first data bind.
    cloud_list_ = nullptr;
    if (!ctrl_->getConsoles().empty()) {
        refreshConsoles();
    } else {
        setConsoleListMessage(ctrl_->hasCredentials()
            ? brls::getStr("lunarnx/main/press_find_xbox")
            : brls::getStr("lunarnx/main/sign_in_then_xbox"));
    }

    auto* status_card = makeUiCard(brls::Axis::ROW);
    status_card->setHeight(58);
    status_card->setPadding(12, 18, 12, 18);
    auto* status_mark = new brls::Label();
    status_mark->setText(brls::getStr("lunarnx/common/status"));
    status_mark->setWidth(88);
    status_mark->setFontSize(12);
    status_mark->setTextColor(p.accent);
    status_card->addView(status_mark);
    status_ = new brls::Label();
    status_->setText("");
    status_->setFontSize(14);
    status_->setTextColor(p.text_muted);
    status_->setGrow(1.0f);
    status_->setVerticalAlign(brls::VerticalAlign::CENTER);
    status_card->addView(status_);
    root->addView(status_card);
    installStateCallback();

    // Do not auto-start home stream in mock mode.
    // Auto-connect races WebRTC setup during first frame layout and can crash
    // Ryubing before the user can open the xCloud library UI.
    if (ctrl_->isMockMode()) {
        lunar::diagnosticLog("ui-main",
                             "Mock mode ready consoles=%zu (auto-connect disabled)",
                             ctrl_->getConsoles().size());
        if (status_) {
            status_->setText(brls::getStr("lunarnx/main/mock_ready"));
        }
    }
    return makeAppFrame("Xbox", workspace);
}

void MainActivity::onResume() {
    installStateCallback();
}

void MainActivity::installStateCallback() {
    auto alive = alive_;
    auto* status = status_;
    ctrl_->setStateCallback([alive, status](app::StreamState state,
                                            const std::string& info) {
        brls::sync([alive, status, text = streamStateText(state, info)]() {
            if (!alive->load() || !status) return;
            status->setText(text);
        });
    });
}

void MainActivity::openStreamSettings() {
    signout_pending_ = false;

    StreamSettingsSnapshot snapshot;
    snapshot.width = stream_width_;
    snapshot.height = stream_height_;
    snapshot.bitrate_kbps = stream_bitrate_kbps_;
    snapshot.preferred_game_language = preferred_game_language_;
    snapshot.video_backend = video_backend_;
    snapshot.post_process_mode = post_process_mode_;
    snapshot.dithering_enabled = dithering_enabled_;
    snapshot.vibration_enabled = vibration_enabled_;
    snapshot.rumble_strength_percent = rumble_strength_percent_;

    auto alive = alive_;
    brls::Application::pushActivity(
        new StreamSettingsActivity(
            ctrl_, snapshot,
            [this, alive](const StreamSettingsSnapshot& updated) {
                if (!alive->load()) return;
                const bool game_language_changed =
                    preferred_game_language_ != updated.preferred_game_language;
                stream_width_ = updated.width;
                stream_height_ = updated.height;
                stream_bitrate_kbps_ = updated.bitrate_kbps;
                preferred_game_language_ = updated.preferred_game_language;
                ctrl_->setPreferredGameLanguage(preferred_game_language_);
                video_backend_ = updated.video_backend;
                post_process_mode_ = updated.post_process_mode;
                dithering_enabled_ = updated.dithering_enabled;
                vibration_enabled_ = updated.vibration_enabled;
                rumble_strength_percent_ = updated.rumble_strength_percent;
                if (status_) {
                    status_->setText(brls::getStr(
                        "lunarnx/main/settings_ready",
                        stream_height_,
                        stream::videoBackendOverlayName(video_backend_)));
                }
                if (game_language_changed &&
                    stream_source_ == StreamSource::Cloud) {
                    refreshCurrentSource();
                }
            }),
        brls::TransitionAnimation::NONE);
}

void MainActivity::refreshCurrentSource() {
    signout_pending_ = false;
    if (fetching_cloud_->load() || fetching_consoles_->load()) return;

    if (stream_source_ == StreamSource::Cloud) {
        lunar::diagnosticLog(
            "ui-main", "Find xCloud click has_credentials=%s cloud=%s fetching=%s",
            ctrl_->hasCredentials() ? "true" : "false",
            ctrl_->hasCloudAccess() ? "true" : "false",
            fetching_cloud_->load() ? "true" : "false");
        if (!ctrl_->hasCredentials()) {
            if (status_) status_->setText(brls::getStr("lunarnx/main/sign_in_cloud"));
            return;
        }
        if (fetching_cloud_->exchange(true)) return;

        if (status_) status_->setText(brls::getStr("lunarnx/main/loading_cloud"));
        setFetchControlsEnabled(false);
        setListLoading(
            ctrl_->hasCloudAccess()
                ? brls::getStr("lunarnx/main/loading_cloud_library")
                : brls::getStr("lunarnx/main/refreshing_cloud_access"),
            brls::getStr("lunarnx/main/cloud_fetch_detail"));

        auto alive = alive_;
        auto ctrl = ctrl_;
        auto fetching = fetching_cloud_;
        auto* self = this;
        bool started = lunar::platform::startNetworkWorker(
            "find-xcloud", [alive, ctrl, fetching, self]() {
                bool ok = ctrl->fetchCloudTitles(true);
                std::string error = ctrl->getCloudTitleFetchError();
                const bool used_cached_fallback = ok && !error.empty();
                const bool restored_cache = !ok && ctrl->restoreCloudTitlesFromCache();
                brls::sync([self, alive, fetching, ok, used_cached_fallback,
                            restored_cache,
                            error = std::move(error)]() {
                    fetching->store(false);
                    if (!alive->load()) return;
                    self->setFetchControlsEnabled(true);
                    lunar::diagnosticLog("ui-main", "Find xCloud UI update ok=%s",
                                         ok ? "true" : "false");
                    if (self->stream_source_ != StreamSource::Cloud) return;
                    if (ok || restored_cache) {
                        self->refreshCloudTitles();
                        if ((used_cached_fallback || restored_cache) &&
                            self->status_) {
                            self->status_->setText(brls::getStr(
                                "lunarnx/main/cloud_cache_refresh_failed"));
                        }
                    } else {
                        if (self->status_) {
                            self->status_->setText(
                                brls::getStr("lunarnx/main/load_cloud_failed"));
                        }
                        self->setConsoleListMessage(
                            error.empty()
                                ? brls::getStr("lunarnx/main/cloud_entitlement_failed")
                                : error);
                    }
                });
            });
        if (!started) {
            fetching_cloud_->store(false);
            setFetchControlsEnabled(true);
            if (status_) {
                status_->setText(brls::getStr("lunarnx/main/cloud_request_failed"));
            }
            setConsoleListMessage(brls::getStr("lunarnx/main/network_worker_failed"));
        }
        return;
    }

    lunar::diagnosticLog(
        "ui-main", "Find My Xbox click has_credentials=%s mock=%s fetching=%s",
        ctrl_->hasCredentials() ? "true" : "false",
        ctrl_->isMockMode() ? "true" : "false",
        fetching_consoles_->load() ? "true" : "false");
    if (!ctrl_->hasCredentials()) {
        if (status_) status_->setText(brls::getStr("lunarnx/main/sign_in_xbox"));
        return;
    }
    if (fetching_consoles_->exchange(true)) return;

    if (status_) status_->setText(brls::getStr("lunarnx/main/looking_consoles"));
    setFetchControlsEnabled(false);
    setListLoading(brls::getStr("lunarnx/main/searching_consoles"),
                   brls::getStr("lunarnx/main/console_fetch_detail"));

    auto alive = alive_;
    auto ctrl = ctrl_;
    auto fetching = fetching_consoles_;
    auto* self = this;
    bool started = lunar::platform::startNetworkWorker(
        "find-xbox", [alive, ctrl, fetching, self]() {
            bool ok = ctrl->fetchConsoles();
            std::string error = ctrl->getConsoleFetchError();
            lunar::diagnosticLog("ui-main", "Find My Xbox worker done ok=%s error=%s",
                                 ok ? "true" : "false", error.c_str());
            brls::sync([self, alive, fetching, ok, error = std::move(error)]() {
                fetching->store(false);
                if (!alive->load()) return;
                self->setFetchControlsEnabled(true);
                if (self->stream_source_ != StreamSource::Xbox) return;
                if (ok) {
                    self->refreshConsoles();
                } else {
                    if (self->status_) {
                        self->status_->setText(
                            brls::getStr("lunarnx/main/load_consoles_failed"));
                    }
                    self->setConsoleListMessage(
                        error.empty()
                            ? brls::getStr("lunarnx/main/consoles_empty")
                            : error);
                }
            });
        });
    if (!started) {
        fetching_consoles_->store(false);
        setFetchControlsEnabled(true);
        if (status_) {
            status_->setText(brls::getStr("lunarnx/main/console_request_failed"));
        }
        setConsoleListMessage(brls::getStr("lunarnx/main/network_worker_failed"));
    }
}

void MainActivity::startConsoleStream(const std::string& server_id,
                                      const std::string& console_name) {
    signout_pending_ = false;
    if (!status_) return;
    if (connecting_->exchange(true)) {
        lunar::diagnosticLog("ui-main", "Connect ignored: startup already active");
        return;
    }
    const auto stream_state = ctrl_->getState();
    if (stream_state == app::StreamState::Connecting ||
        stream_state == app::StreamState::Streaming) {
        connecting_->store(false);
        lunar::diagnosticLog("ui-main", "Connect ignored state=%d connecting=%s",
                             static_cast<int>(stream_state),
                             connecting_->load() ? "true" : "false");
        return;
    }

    PosterLoader::instance().beginBatch();

    lunar::diagnosticLog("ui-main", "Connect begin server=%s name=%s",
                         server_id.c_str(), console_name.c_str());
    status_->setText(brls::getStr("lunarnx/main/connecting_to", console_name));

    StreamLaunchRequest request;
    request.target = StreamLaunchTarget::HomeConsole;
    request.target_id = server_id;
    request.display_name = console_name;
    request.width = stream_width_;
    request.height = stream_height_;
    request.bitrate_kbps = stream_bitrate_kbps_;
    request.options.video_backend = video_backend_;
    request.options.post_process_mode = post_process_mode_;
    request.options.dithering_enabled = dithering_enabled_;

    auto alive = alive_;
    auto connecting = connecting_;
    auto* status = status_;
    auto completion = [alive, connecting, status, console_name](
                          StreamLaunchResult result, const std::string& detail) {
        connecting->store(false);
        if (!alive->load() || !status) return;
        if (result == StreamLaunchResult::Started) {
            status->setText(brls::getStr("lunarnx/main/streaming_title", console_name));
        } else if (result == StreamLaunchResult::Cancelled) {
            status->setText(brls::getStr("lunarnx/main/startup_cancelled"));
        } else {
            status->setText(detail.empty()
                ? brls::getStr("lunarnx/main/console_connection_failed", console_name)
                : brls::getStr("lunarnx/main/console_retry_detail", detail));
        }
    };

    brls::Application::pushActivity(
        new StreamLoadingActivity(ctrl_, std::move(request), std::move(completion)),
        brls::TransitionAnimation::NONE);
}

void MainActivity::refreshConsoles() {
    if (!console_list_) return;
    const auto& p = uiPalette();
    setCloudListVisible(false);
    console_list_->setVisibility(brls::Visibility::VISIBLE);
    PosterLoader::instance().beginBatch();
    console_list_->clearViews();

    auto consoles = ctrl_->getConsoles();
    if (consoles.empty()) {
        setConsoleListMessage(ctrl_->getConsoleFetchError().empty()
            ? brls::getStr("lunarnx/main/no_consoles")
            : ctrl_->getConsoleFetchError());
        return;
    }

    if (status_) {
        status_->setText(brls::getStr("lunarnx/main/consoles_found",
                                      consoles.size()));
    }

    for (auto& c : consoles) {
        bool on = c.power_state == "On";
        bool can_connect = on || c.power_state == "ConnectedStandby";

        auto* item = makeUiCard(brls::Axis::ROW);
        item->setHeight(108);
        item->setPadding(10, 18, 10, 18);
        item->setMarginBottom(12);
        item->setAlignItems(brls::AlignItems::CENTER);
        item->setFocusable(can_connect);
        item->setHighlightCornerRadius(10);
        item->addView(new ConsoleGlyphView(c.console_type, can_connect));

        auto* details = new brls::Box(brls::Axis::COLUMN);
        details->setGrow(1.0f);
        details->setPadding(6, 20, 6, 20);

        auto* name = new brls::Label();
        name->setText(c.name.empty()
            ? brls::getStr("lunarnx/main/xbox_console")
            : c.name);
        name->setFontSize(23);
        name->setTextColor(p.text);
        name->setSingleLine(true);
        name->setVerticalAlign(brls::VerticalAlign::CENTER);
        details->addView(name);

        auto* meta = makeMutedLabel(
                (c.console_type.empty() ? "Xbox" : c.console_type) +
                "  /  " +
                (on ? brls::getStr("lunarnx/common/ready")
                    : (can_connect ? brls::getStr("lunarnx/common/standby")
                                   : brls::getStr("lunarnx/common/offline"))),
            14);
        meta->setHeight(28);
        meta->setVerticalAlign(brls::VerticalAlign::CENTER);
        details->addView(meta);

        auto* description = makeMutedLabel(
            can_connect
                ? brls::getStr("lunarnx/main/console_ready_description")
                : brls::getStr("lunarnx/main/console_offline_description"),
            13);
        description->setSingleLine(true);
        details->addView(description);
        item->addView(details);

        std::string server_id = c.id;
        std::string console_name = c.name.empty()
            ? brls::getStr("lunarnx/main/xbox_console")
            : c.name;
        if (can_connect) {
            item->registerClickAction(
                [this, server_id, console_name](brls::View*) -> bool {
                    startConsoleStream(server_id, console_name);
                    return true;
                });
        }

        console_list_->addView(item);
    }
}

void MainActivity::setConsoleListMessage(const std::string& message) {
    if (!console_list_) return;
    setCloudListVisible(false);
    console_list_->setVisibility(brls::Visibility::VISIBLE);
    PosterLoader::instance().beginBatch();
    console_list_->clearViews();

    const auto& p = uiPalette();
    auto* card = makeUiCard(brls::Axis::COLUMN);
    card->setHeight(148);
    card->setJustifyContent(brls::JustifyContent::CENTER);
    card->setAlignItems(brls::AlignItems::CENTER);

    auto* eyebrow = new brls::Label();
    eyebrow->setText(stream_source_ == StreamSource::Cloud
        ? brls::getStr("lunarnx/main/cloud_section")
        : brls::getStr("lunarnx/main/remote_section"));
    eyebrow->setFontSize(12);
    eyebrow->setTextColor(p.accent);
    eyebrow->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    card->addView(eyebrow);

    auto* label = new brls::Label();
    label->setText(message);
    label->setFontSize(14);
    label->setTextColor(p.text_muted);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    card->addView(label);
    console_list_->addView(card);
}

void MainActivity::setListLoading(const std::string& title,
                                  const std::string& detail) {
    if (!console_list_) return;
    setCloudListVisible(false);
    console_list_->setVisibility(brls::Visibility::VISIBLE);
    PosterLoader::instance().beginBatch();
    console_list_->clearViews();

    const auto& p = uiPalette();
    auto* card = makeUiCard(brls::Axis::ROW);
    card->setHeight(168);
    card->setPadding(24, 30, 24, 30);
    card->setAlignItems(brls::AlignItems::CENTER);

    auto* spinner = new brls::ProgressSpinner();
    spinner->setWidth(76);
    spinner->setHeight(76);
    card->addView(spinner);

    auto* copy = new brls::Box(brls::Axis::COLUMN);
    copy->setGrow(1.0f);
    copy->setPadding(8, 0, 8, 28);

    auto* eyebrow = new brls::Label();
    eyebrow->setText(stream_source_ == StreamSource::Cloud
        ? brls::getStr("lunarnx/main/cloud_section")
        : brls::getStr("lunarnx/main/remote_section"));
    eyebrow->setFontSize(12);
    eyebrow->setTextColor(p.accent);
    copy->addView(eyebrow);

    auto* title_label = new brls::Label();
    title_label->setText(title);
    title_label->setFontSize(20);
    title_label->setTextColor(p.text);
    copy->addView(title_label);

    auto* detail_label = new brls::Label();
    detail_label->setText(detail);
    detail_label->setFontSize(13);
    detail_label->setTextColor(p.text_muted);
    copy->addView(detail_label);

    card->addView(copy);
    console_list_->addView(card);
}

void MainActivity::setFetchControlsEnabled(bool enabled) {
    const auto state = enabled
        ? brls::ButtonState::ENABLED
        : brls::ButtonState::DISABLED;
    if (refresh_btn_) refresh_btn_->setState(state);
    if (cloud_search_btn_) cloud_search_btn_->setState(state);
}

void MainActivity::confirmSignOut() {
    auto* dialog = new brls::Dialog(
        brls::getStr("lunarnx/common/sign_out_confirm_message"));
    dialog->addButton(brls::getStr("lunarnx/common/cancel"), []() {});
    dialog->addButton(brls::getStr("lunarnx/common/sign_out"),
        [this]() { resetToAuthActivity(); });
    dialog->open();
}

void MainActivity::resetToAuthActivity() {
    if (signout_running_.exchange(true)) return;
    alive_->store(false);
    if (status_) status_->setText(brls::getStr("lunarnx/main/signing_out"));

    auto ctrl = ctrl_;
    bool started = lunar::platform::startNetworkWorker("sign-out", [ctrl]() {
        ctrl->signOut();
        brls::sync([]() {
            // Return from the Xbox account scope to platform selection.
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
        });
    });
    if (!started) {
        signout_running_ = false;
        if (status_) {
            status_->setText(brls::getStr("lunarnx/main/sign_out_worker_failed"));
        }
    }
}



void MainActivity::setStreamSource(StreamSource source) {
    signout_pending_ = false;
    stream_source_ = source;
    highlightStreamSource();
    updateRefreshButtonLabel();
    updateCloudSearchButtonLabel();
    if (!console_list_) return;

    if (source == StreamSource::Cloud) {
        if (!ctrl_->getCloudTitles().empty()) {
            refreshCloudTitles();
            return;
        }
        if (ctrl_->restoreCloudTitlesFromCache()) {
            refreshCloudTitles();
            if (status_) {
                status_->setText(brls::getStr("lunarnx/main/cloud_cache_loaded"));
            }
            return;
        }
        if (!ctrl_->hasCloudAccess()) {
            setConsoleListMessage(brls::getStr("lunarnx/main/xcloud_token_missing"));
            return;
        }
        setConsoleListMessage(brls::getStr("lunarnx/main/press_find_xcloud"));
        return;
    }

    // Xbox home
    if (!ctrl_->getConsoles().empty()) {
        refreshConsoles();
    } else {
        setConsoleListMessage(ctrl_->hasCredentials()
            ? brls::getStr("lunarnx/main/press_find_xbox")
            : brls::getStr("lunarnx/main/sign_in_then_xbox"));
    }
}

void MainActivity::highlightStreamSource() {
    if (source_xbox_) {
        source_xbox_->setState(brls::ButtonState::ENABLED);
        setSidebarButtonActive(source_xbox_,
            stream_source_ == StreamSource::Xbox);
    }
    if (source_cloud_) {
        source_cloud_->setState(brls::ButtonState::ENABLED);
        setSidebarButtonActive(source_cloud_,
            stream_source_ == StreamSource::Cloud);
    }
}

void MainActivity::updateRefreshButtonLabel() {
    if (refresh_btn_) {
        refresh_btn_->setText(stream_source_ == StreamSource::Cloud
            ? brls::getStr("lunarnx/main/refresh_library")
            : brls::getStr("lunarnx/main/find_xbox"));
    }
    if (content_title_) {
        content_title_->setText(stream_source_ == StreamSource::Cloud
            ? brls::getStr("lunarnx/common/xbox_cloud_gaming")
            : brls::getStr("lunarnx/main/your_consoles"));
    }
    if (content_subtitle_) {
        content_subtitle_->setText(stream_source_ == StreamSource::Cloud
            ? brls::getStr("lunarnx/main/cloud_subtitle")
            : brls::getStr("lunarnx/main/console_subtitle"));
    }
    updateCloudSearchButtonLabel();
}

void MainActivity::updateCloudSearchButtonLabel() {
    if (!cloud_search_btn_) return;
    if (stream_source_ != StreamSource::Cloud) {
        cloud_search_btn_->setVisibility(brls::Visibility::GONE);
        return;
    }
    cloud_search_btn_->setVisibility(brls::Visibility::VISIBLE);
    if (cloud_search_query_.empty()) {
        cloud_search_btn_->setText(brls::getStr("lunarnx/main/search_library"));
    } else {
        cloud_search_btn_->setText(
            brls::getStr("lunarnx/main/search_query", cloud_search_query_));
    }
}

void MainActivity::setCloudListVisible(bool visible) {
    if (console_list_) {
        console_list_->setVisibility(brls::Visibility::VISIBLE);
    }
}

void MainActivity::rebuildCloudList() {
    lunar::diagnosticLog("ui-main", "rebuildCloudList begin");
    if (!console_list_) {
        lunar::diagnosticLog("ui-main", "rebuildCloudList missing console_list");
        return;
    }

    const auto& palette = uiPalette();
    const auto poster_batch = PosterLoader::instance().beginBatch();
    console_list_->clearViews();
    console_list_->setVisibility(brls::Visibility::VISIBLE);

    auto titles = ctrl_->getCloudTitles();
    auto recent = ctrl_->getRecentCloudTitles();
    auto newly = ctrl_->getNewCloudTitles();
    lunar::diagnosticLog("ui-main", "rebuildCloudList data all=%zu recent=%zu new=%zu",
                         titles.size(), recent.size(), newly.size());

    // Testing helper: ensure Fortnite (F2P xCloud) is playable even when not in
    // the current recent/full library payload.
    auto has_fortnite = [](const std::vector<lunar::api::CloudTitle>& list) {
        for (const auto& t : list) {
            const std::string id = t.title_id;
            const std::string name = t.name;
            if (id == "FORTNITE" || name.find("Fortnite") != std::string::npos ||
                name.find("fortnite") != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    if (!ctrl_->isMockMode() && !has_fortnite(titles) && !has_fortnite(recent)) {
        lunar::api::CloudTitle fn;
        fn.title_id = "FORTNITE";
        fn.name = "Fortnite (F2P test)";
        fn.publisher = "Epic Games";
        fn.product_id = "BT5P2X999VH2";
        recent.insert(recent.begin(), fn);
        titles.insert(titles.begin(), fn);
        lunar::diagnosticLog("ui-main", "Injected Fortnite F2P test entry");
    }


    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    };
    if (!cloud_search_query_.empty()) {
        const std::string query = lower(cloud_search_query_);
        titles.erase(
            std::remove_if(titles.begin(), titles.end(),
                [&lower, &query](const lunar::api::CloudTitle& title) {
                    return lower(title.name + " " + title.publisher).find(query) ==
                           std::string::npos;
                }),
            titles.end());
        recent.clear();
        newly.clear();
    }

    if (status_) {
        status_->setText(cloud_search_query_.empty()
            ? brls::getStr("lunarnx/main/library_count", titles.size())
            : brls::getStr("lunarnx/main/search_count", titles.size()));
    }

    if (titles.empty() && recent.empty() && newly.empty()) {
        setConsoleListMessage(cloud_search_query_.empty()
            ? brls::getStr("lunarnx/main/no_cloud_titles")
            : brls::getStr("lunarnx/main/no_search_matches", cloud_search_query_));
        return;
    }

    // Collect image views for deferred poster load after list is on screen.
    struct PendingPoster {
        brls::Image* image = nullptr;
        std::string url;
    };
    auto pending = std::make_shared<std::vector<PendingPoster>>();

    auto make_title_card = [this, pending, &palette](
                               const lunar::api::CloudTitle& title,
                               bool load_poster) -> brls::Box* {
        auto* card = makeUiCard(brls::Axis::COLUMN);
        card->setWidth(240);
        card->setHeight(250);
        card->setPadding(12, 12, 12, 12);
        card->setFocusable(!title.title_id.empty());
        card->setHighlightCornerRadius(10);

        auto* image = new brls::Image();
        image->setWidth(216);
        image->setHeight(174);
        image->setCornerRadius(8);
        image->setBackgroundColor(palette.surface_alt);
        image->setScalingType(brls::ImageScalingType::FILL);
        card->addView(image);
        if (load_poster && !title.image_url.empty()) {
            pending->push_back(PendingPoster{image, title.image_url});
        }

        const std::string title_name = title.name.empty() ? title.title_id : title.name;
        auto* details = new brls::Box(brls::Axis::COLUMN);
        details->setGrow(1.0f);
        details->setHeight(52);
        details->setPadding(8, 0, 0, 0);

        auto* name = new brls::Label();
        name->setText(title_name);
        name->setFontSize(19);
        name->setTextColor(palette.text);
        name->setSingleLine(true);
        details->addView(name);

        const std::string title_id = title.title_id;
        if (!title_id.empty()) {
            card->registerClickAction(
            [this, title_id, title_name](brls::View*) -> bool {
                startCloudTitleStream(title_id, title_name);
                return true;
            });
        }
        card->addView(details);
        return card;
    };

    auto add_section = [this, &make_title_card](
                           const std::string& title,
                           const std::string& subtitle,
                           const std::vector<lunar::api::CloudTitle>& items,
                           size_t max_items,
                           size_t max_posters) {
        if (items.empty()) return;
        console_list_->addView(makeSectionHeader(title, subtitle));
        const size_t shown = std::min(items.size(), max_items);
        for (size_t index = 0; index < shown; index += 2) {
            auto* row = new brls::Box(brls::Axis::ROW);
            row->setHeight(264);
            row->addView(make_title_card(items[index], index < max_posters));
            if (index + 1 < shown) {
                auto* second = make_title_card(
                    items[index + 1], index + 1 < max_posters);
                second->setMarginLeft(12);
                row->addView(second);
            }
            console_list_->addView(row);
        }
        if (items.size() > shown) {
            auto* more = makeMutedLabel(
                brls::getStr("lunarnx/main/more_titles", items.size() - shown),
                13);
            more->setHeight(32);
            more->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
            console_list_->addView(more);
        }
    };

    if (!cloud_search_query_.empty()) {
        add_section(brls::getStr("lunarnx/main/search_results"),
                    brls::getStr("lunarnx/main/search_results_subtitle"),
                    titles, 12, 6);
    } else {
        add_section(brls::getStr("lunarnx/main/jump_back_in"),
                    brls::getStr("lunarnx/main/jump_back_subtitle"),
                    recent, 6, 6);
        add_section(brls::getStr("lunarnx/main/new_xcloud"),
                    brls::getStr("lunarnx/main/new_xcloud_subtitle"),
                    newly, 4, 4);
        add_section(brls::getStr("lunarnx/main/all_games"),
                    brls::getStr("lunarnx/main/all_games_subtitle"),
                    titles, 12, 4);
    }

    lunar::diagnosticLog("ui-main", "rebuildCloudList rows built pending_posters=%zu",
                         pending->size());

    // Enqueue posters after list is built. PosterLoader runs them one-by-one.
    auto alive = alive_;
    brls::delay(50, [alive, pending, poster_batch]() {
        if (!alive->load()) return;
        lunar::diagnosticLog("ui-main", "enqueue posters count=%zu", pending->size());
        for (size_t i = 0; i < pending->size(); ++i) {
            const auto& poster = (*pending)[i];
            if (!poster.image || poster.url.empty()) continue;
            lunar::diagnosticLog("ui-main", "poster enqueue %zu %s", i,
                                 poster.url.c_str());
            PosterLoader::instance().load(poster.image, poster.url, poster_batch);
        }
    });

    if (status_) status_->setText(brls::getStr("lunarnx/main/library_loaded"));
    lunar::diagnosticLog("ui-main", "rebuildCloudList done");
}

void MainActivity::refreshCloudTitles() {
    lunar::diagnosticLog("ui-main", "refreshCloudTitles begin");
    if (!console_list_) {
        lunar::diagnosticLog("ui-main", "refreshCloudTitles missing console_list");
        return;
    }
    auto titles = ctrl_->getCloudTitles();
    lunar::diagnosticLog("ui-main", "refreshCloudTitles titles=%zu", titles.size());
    if (titles.empty()) {
        setConsoleListMessage(ctrl_->getCloudTitleFetchError().empty()
            ? brls::getStr("lunarnx/main/no_cloud_found")
            : ctrl_->getCloudTitleFetchError());
        return;
    }
    rebuildCloudList();
    lunar::diagnosticLog("ui-main", "refreshCloudTitles end");
}

void MainActivity::promptCloudSearch() {
    // Swkbd on Switch; plain empty-clear if dialog cancelled.
    brls::Application::getImeManager()->openForText(
        [this](const std::string& text) {
            cloud_search_query_ = text;
            // trim
            while (!cloud_search_query_.empty() &&
                   std::isspace(static_cast<unsigned char>(cloud_search_query_.front()))) {
                cloud_search_query_.erase(cloud_search_query_.begin());
            }
            while (!cloud_search_query_.empty() &&
                   std::isspace(static_cast<unsigned char>(cloud_search_query_.back()))) {
                cloud_search_query_.pop_back();
            }
            updateCloudSearchButtonLabel();
            if (stream_source_ == StreamSource::Cloud) {
                refreshCloudTitles();
            }
        },
        brls::getStr("lunarnx/main/search_ime_title"),
        brls::getStr("lunarnx/main/search_ime_hint"),
        64,
        cloud_search_query_,
        0);
}

void MainActivity::startCloudTitleStream(const std::string& title_id,
                                         const std::string& title_name) {
    lunar::diagnosticLog("ui-main", "startCloudTitleStream begin id=%s name=%s",
                         title_id.c_str(), title_name.c_str());
    signout_pending_ = false;
    if (!status_) return;
    if (connecting_->exchange(true)) {
        lunar::diagnosticLog("ui-main", "xCloud Play ignored: startup already active");
        return;
    }
    const auto stream_state = ctrl_->getState();
    if (stream_state == app::StreamState::Connecting ||
        stream_state == app::StreamState::Streaming) {
        connecting_->store(false);
        return;
    }

    PosterLoader::instance().beginBatch();

    status_->setText(brls::getStr("lunarnx/main/starting_xcloud", title_name));

    StreamLaunchRequest request;
    request.target = StreamLaunchTarget::CloudTitle;
    request.target_id = title_id;
    request.display_name = title_name;
    request.width = stream_width_;
    request.height = stream_height_;
    request.bitrate_kbps = stream_bitrate_kbps_;
    request.options.video_backend = video_backend_;
    request.options.post_process_mode = post_process_mode_;
    request.options.dithering_enabled = dithering_enabled_;

    auto alive = alive_;
    auto connecting = connecting_;
    auto* status = status_;
    auto completion = [alive, connecting, status, title_name](
                          StreamLaunchResult result, const std::string& detail) {
        connecting->store(false);
        if (!alive->load() || !status) return;
        if (result == StreamLaunchResult::Started) {
            status->setText(brls::getStr("lunarnx/main/streaming_title", title_name));
        } else if (result == StreamLaunchResult::Cancelled) {
            status->setText(brls::getStr("lunarnx/main/xcloud_cancelled"));
        } else {
            status->setText(detail.empty()
                ? brls::getStr("lunarnx/main/xcloud_failed", title_name)
                : brls::getStr("lunarnx/main/retry_detail", detail));
        }
    };

    brls::Application::pushActivity(
        new StreamLoadingActivity(ctrl_, std::move(request), std::move(completion)),
        brls::TransitionAnimation::NONE);
}

} // namespace lunar::ui
#endif
