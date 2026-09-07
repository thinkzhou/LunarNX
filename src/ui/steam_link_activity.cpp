#ifdef __SWITCH__

#include "steam_link_activity.h"

#include "grid_navigation.h"
#include "ui_style.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"
#include "stream_view.h"

#include <switch/kernel/random.h>

#include <algorithm>
#include <exception>

namespace lunar::ui {
namespace {

} // namespace

SteamLinkPairingActivity::SteamLinkPairingActivity(
    std::shared_ptr<steamlink::SteamLinkClient> client,
    steamlink::SteamLinkHost host)
    : client_(std::move(client)), host_(std::move(host)) {}

SteamLinkPairingActivity::~SteamLinkPairingActivity() {
    alive_->store(false);
    if (pending_runtime_) pending_runtime_->requestStop();
    if (client_ && authorizing_) client_->cancelAuthorization();
}

brls::View* SteamLinkPairingActivity::createContentView() {
    const auto& p = uiPalette();
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
    root->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
    root->setPadding(30, 70, 24, 70);
    root->setBackgroundColor(p.background);
    root->setAlignItems(brls::AlignItems::CENTER);
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->registerAction(brls::getStr("lunarnx/common/cancel"),
        brls::ControllerButton::BUTTON_B, [](brls::View*) -> bool {
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    auto* card = makeUiCard(brls::Axis::COLUMN);
    card->setWidth(760);
    card->setHeight(510);
    card->setPadding(24, 36, 24, 36);
    card->setAlignItems(brls::AlignItems::CENTER);

    auto* details = new brls::Box(brls::Axis::COLUMN);
    details->setWidth(680);
    details->setHeight(430);
    details->setJustifyContent(brls::JustifyContent::CENTER);
    details->setAlignItems(brls::AlignItems::CENTER);
    auto* title = new brls::Label();
    title->setText(host_.hostname.empty() ? host_.address : host_.hostname);
    title->setFontSize(26);
    title->setTextColor(p.text);
    title->setHeight(52);
    details->addView(title);
    auto* address = makeMutedLabel(host_.address + ":" + std::to_string(host_.port), 14);
    address->setHeight(36);
    details->addView(address);
    pin_display_ = new brls::Label();
    pin_display_->setFontSize(28);
    pin_display_->setTextColor(p.text);
    pin_display_->setWidth(560);
    pin_display_->setHeight(68);
    pin_display_->setMarginTop(18);
    pin_display_->setBackgroundColor(p.surface_alt);
    pin_display_->setBorderThickness(1);
    pin_display_->setBorderColor(p.border);
    pin_display_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    pin_display_->setVerticalAlign(brls::VerticalAlign::CENTER);
    details->addView(pin_display_);
    status_ = new brls::Label();
    status_->setText(brls::getStr("lunarnx/steam_link/pin_help"));
    status_->setFontSize(14);
    status_->setTextColor(p.text_muted);
    status_->setWidth(560);
    status_->setHeight(72);
    status_->setIsWrapping(true);
    status_->setVerticalAlign(brls::VerticalAlign::CENTER);
    details->addView(status_);

    security_pin_input_ = new brls::InputCell();
    security_pin_input_->setWidth(560);
    security_pin_input_->setHeight(58);
    security_pin_input_->setMarginTop(10);
    security_pin_input_->init(
        "Steam host security PIN",
        "",
        [this](std::string text) { security_pin_ = std::move(text); },
        "The PIN configured in Steam Remote Play; leave empty to try without one",
        "Example: 1234", 15, 0);
    details->addView(security_pin_input_);

    stream_button_ = new brls::Button();
    stream_button_->setWidth(300);
    stream_button_->setHeight(54);
    stream_button_->setMarginTop(12);
    stream_button_->setText("Start Steam stream");
    stream_button_->setFocusable(false);
    stylePrimaryButton(stream_button_);
    stream_button_->registerClickAction([this](brls::View*) -> bool {
        startStream();
        return true;
    });
    details->addView(stream_button_);
    card->addView(details);
    root->addView(card);
    uint32_t random_value = 0;
    randomGet(&random_value, sizeof(random_value));
    char pin[5]{};
    std::snprintf(pin, sizeof(pin), "%04u", random_value % 10000);
    pairing_pin_ = pin;
    pin_display_->setText(pairing_pin_);
    return root;
}

void SteamLinkPairingActivity::onContentAvailable() {
    startAuthorization();
}

void SteamLinkPairingActivity::startAuthorization() {
    if (authorizing_ || !client_ || pairing_pin_.size() != 4) return;
    authorizing_ = true;
    status_->setText(brls::getStr("lunarnx/steam_link/pairing"));
    auto alive = alive_;
    if (!client_->authorize(host_, pairing_pin_, [this, alive](bool success, const std::string& error) {
        brls::sync([this, alive, success, error]() {
            if (!alive->load()) return;
            authorizing_ = false;
            if (success) {
                status_->setText(brls::getStr("lunarnx/steam_link/pair_success"));
                if (stream_button_) stream_button_->setFocusable(true);
                lunar::persistentEventLog("steam-link-ui", "pair success host=%s",
                                          host_.address.c_str());
            } else {
                status_->setText(error.empty()
                    ? brls::getStr("lunarnx/steam_link/pair_failed") : error);
            }
        });
    })) {
        authorizing_ = false;
        status_->setText(client_->lastError());
    }
}

void SteamLinkPairingActivity::startStream() {
    if (starting_stream_ || authorizing_ || !client_ || !client_->isAuthorized(host_.client_id)) {
        return;
    }
    starting_stream_ = true;
    if (stream_button_) stream_button_->setFocusable(false);
    if (security_pin_input_) security_pin_input_->setFocusable(false);
    if (status_) status_->setText("Requesting Steam stream...");
    auto runtime = std::make_shared<steamlink::SteamLinkStreamController>(
        client_, host_, security_pin_, 1280, 720);
    pending_runtime_ = runtime;
    auto alive = alive_;
    if (!lunar::platform::startNetworkWorker("steam-link-stream",
            [this, alive, runtime]() {
                bool ok = false;
                std::string error;
                try {
                    ok = runtime->startStream();
                    if (!ok) error = runtime->lastError();
                } catch (const std::exception& e) {
                    error = std::string("Steam stream exception: ") + e.what();
                    lunar::persistentEventLog("steam-link-ui", "stream exception detail=%s", e.what());
                } catch (...) {
                    error = "Unexpected Steam stream exception";
                    lunar::persistentEventLog("steam-link-ui", "stream unknown exception");
                }
                brls::sync([this, alive, runtime, ok, error]() {
                    if (!alive->load()) {
                        if (ok) lunar::platform::startNetworkWorker(
                            "steam-link-orphan-stop", [runtime]() { runtime->stopStream(false); });
                        return;
                    }
                    starting_stream_ = false;
                    pending_runtime_.reset();
                    if (!ok) {
                        if (status_) status_->setText(error.empty() ? "Steam stream failed" : error);
                        if (stream_button_) stream_button_->setFocusable(true);
                        if (security_pin_input_) security_pin_input_->setFocusable(true);
                        return;
                    }
                    lunar::diagnosticLog("steam-link-ui", "stream runtime ready; opening StreamView");
                    brls::Application::popActivity(brls::TransitionAnimation::NONE);
                    brls::Application::pushActivity(
                        new StreamView(runtime), brls::TransitionAnimation::NONE);
                });
            })) {
        starting_stream_ = false;
        if (stream_button_) stream_button_->setFocusable(true);
        if (security_pin_input_) security_pin_input_->setFocusable(true);
        if (status_) status_->setText("Could not start Steam stream worker");
    }
}

SteamLinkActivity::SteamLinkActivity()
    : client_(std::make_shared<steamlink::SteamLinkClient>()) {}

SteamLinkActivity::~SteamLinkActivity() {
    alive_->store(false);
    if (client_) client_->stopDiscovery();
}

brls::View* SteamLinkActivity::createContentView() {
    const auto& p = uiPalette();
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
    root->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
    root->setPadding(30, 60, 24, 60);
    root->setBackgroundColor(p.background);
    root->setAlignItems(brls::AlignItems::CENTER);
    root->registerAction(brls::getStr("lunarnx/common/cancel"),
        brls::ControllerButton::BUTTON_B, [](brls::View*) -> bool {
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });
    auto* heading = makePageHeading(brls::getStr("lunarnx/steam_link/title"));
    root->addView(heading);
    auto* subtitle = makeMutedLabel(brls::getStr("lunarnx/steam_link/subtitle"), 14);
    subtitle->setWidth(920);
    subtitle->setHeight(42);
    subtitle->setIsWrapping(true);
    root->addView(subtitle);
    auto* toolbar = new brls::Box(brls::Axis::ROW);
    toolbar->setWidth(920);
    toolbar->setHeight(62);
    toolbar->setAlignItems(brls::AlignItems::CENTER);
    status_ = new brls::Label();
    status_->setText(brls::getStr("lunarnx/steam_link/discovering"));
    status_->setTextColor(p.text_muted);
    status_->setGrow(1.0f);
    toolbar->addView(status_);
    refresh_button_ = new brls::Button();
    refresh_button_->setText(brls::getStr("lunarnx/steam_link/refresh"));
    refresh_button_->setWidth(160);
    refresh_button_->setHeight(50);
    styleSecondaryButton(refresh_button_);
    refresh_button_->registerClickAction([this](brls::View*) -> bool {
        startDiscovery();
        return true;
    });
    toolbar->addView(refresh_button_);
    root->addView(toolbar);
    host_list_ = new brls::Box(brls::Axis::COLUMN);
    host_list_->setWidth(920);
    host_list_->setGrow(1.0f);
    host_list_->setAlignItems(brls::AlignItems::CENTER);
    root->addView(host_list_);
    return root;
}

void SteamLinkActivity::onContentAvailable() {
    startDiscovery();
}

void SteamLinkActivity::startDiscovery() {
    if (!client_) return;
    if (refresh_button_) refresh_button_->setFocusable(false);
    if (status_) status_->setText(brls::getStr("lunarnx/steam_link/discovering"));
    auto alive = alive_;
    if (!client_->startDiscovery([this, alive](const std::vector<steamlink::SteamLinkHost>& hosts) {
        brls::sync([this, alive, hosts]() {
            if (!alive->load()) return;
            rebuildHosts(hosts);
        });
    })) {
        if (status_) status_->setText(client_->lastError());
        if (refresh_button_) refresh_button_->setFocusable(true);
    }
}

void SteamLinkActivity::rebuildHosts(
    const std::vector<steamlink::SteamLinkHost>& hosts) {
    if (!host_list_) return;
    host_list_->clearViews();
    if (hosts.empty()) {
        host_list_->addView(makeMutedLabel(
            brls::getStr("lunarnx/steam_link/no_hosts"), 16));
    } else {
        for (const auto& host : hosts) {
            auto* card = new brls::Button();
            card->setWidth(920);
            card->setHeight(78);
            card->setMarginBottom(10);
            card->setText((host.hostname.empty() ? host.address : host.hostname) +
                          "  " + host.address + ":" + std::to_string(host.port));
            styleSecondaryButton(card);
            const bool paired = client_->isAuthorized(host.client_id);
            if (paired) card->setText(card->getText() + "  [Paired]");
            card->registerClickAction([this, host](brls::View*) -> bool {
                brls::Application::pushActivity(
                    new SteamLinkPairingActivity(client_, host),
                    brls::TransitionAnimation::NONE);
                return true;
            });
            host_list_->addView(card);
        }
    }
    if (refresh_button_) refresh_button_->setFocusable(true);
}

} // namespace lunar::ui
#endif
