#ifdef __SWITCH__
#include "ps_activity.h"
#include "ps_registration_activity.h"
#include "psn_login_activity.h"
#include "about_activity.h"
#include "ps_settings_activity.h"
#include "stream_settings_activity.h"
#include "stream_view.h"
#include "ui_style.h"
#include "../common.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"
#include "../ps/ps_stream_controller.h"
#include "../ps/psn_auth_manager.h"
#include <algorithm>
#include <cstdio>
#include <thread>

namespace lunar::ui {
namespace {

void appendMockReplayConsole(std::vector<ps::PsConsole>& hosts) {
    if (!ps::psMockReplayEnabled()) return;
    const auto existing = std::find_if(hosts.begin(), hosts.end(),
        [](const ps::PsConsole& host) { return host.stable_id == "mock-replay"; });
    if (existing != hosts.end()) return;

    ps::PsConsole mock;
    mock.stable_id = "mock-replay";
    mock.server_mac = "000000000000";
    mock.nickname = "LunarNX Mock PS5";
    mock.target = 1000100;
    mock.local = ps::PsLocalEndpoint{
        "mock-replay", 0, ps::PsConsoleState::Ready};
    mock.credentials = ps::RegisteredCredential{};
    mock.credentials->server_mac = mock.server_mac;
    mock.credentials->nickname = mock.nickname;
    mock.credentials->target = mock.target;
    hosts.push_back(std::move(mock));
}

struct PsConnectContext {
    std::atomic<bool> alive{true};
    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> connect_worker_done{false};
    std::atomic<bool> cancel_cleanup_started{false};
};

void schedulePsConnectCleanup(
    const std::shared_ptr<PsConnectContext>& context,
    const std::shared_ptr<ps::PsStreamController>& controller) {
    if (!context->cancel_requested.load() ||
        !context->connect_worker_done.load() ||
        context->cancel_cleanup_started.exchange(true)) {
        return;
    }

    diagnosticLog("ui-ps-connect", "cancel cleanup scheduled");
    bool started = lunar::platform::startNetworkWorker(
        "cancel-ps-connect", [context, controller]() {
            diagnosticLog("ui-ps-connect", "cancel cleanup begin");
            controller->setLaunchCallback({});
            controller->setLoginPinCallback({});
            controller->stopStream(false);
            diagnosticLog("ui-ps-connect", "cancel cleanup stream stopped");
            brls::sync([context]() {
                if (!context->alive.load()) return;
                diagnosticLog("ui-ps-connect", "cancel cleanup pop activity");
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
            });
        });
    if (!started) {
        context->cancel_cleanup_started = false;
        diagnosticLog("ui-ps-connect", "cancel cleanup worker failed");
    }
}

class PsConnectActivity : public brls::Activity {
public:
    PsConnectActivity(std::shared_ptr<ps::PsStreamController> controller,
                      std::shared_ptr<ps::PsManager> manager,
                      bool remote,
                      std::string console_name)
        : controller_(std::move(controller))
        , manager_(std::move(manager))
        , remote_(remote)
        , console_name_(std::move(console_name)) {
        diagnosticLog("ui-ps-connect", "constructor name=%s", console_name_.c_str());
    }

    ~PsConnectActivity() override {
        context_->alive = false;
        controller_->setLaunchCallback({});
        controller_->setLoginPinCallback({});
        diagnosticLog("ui-ps-connect", "destructor callbacks cleared");
    }

    brls::View* createContentView() override {
        diagnosticLog("ui-ps-connect", "createContentView begin");
        const auto& p = uiPalette();
        auto* root = new brls::Box(brls::Axis::COLUMN);
        root->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
        root->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
        root->setPadding(30, 48, 24, 48);
        root->setBackgroundColor(p.background);
        root->setAlignItems(brls::AlignItems::CENTER);
        root->setJustifyContent(brls::JustifyContent::CENTER);
        root->setFocusable(true);
        root->setHideHighlight(true);
        root->registerAction("Cancel", brls::ControllerButton::BUTTON_B,
            [this](brls::View*) -> bool {
                cancel();
                return true;
            });

        auto* card = makeUiCard(brls::Axis::COLUMN);
        card->setWidth(780);
        card->setHeight(390);
        card->setAlignItems(brls::AlignItems::CENTER);
        auto* eyebrow = new brls::Label();
        eyebrow->setText(brls::getStr("lunarnx/ps/connect_eyebrow"));
        eyebrow->setFontSize(14);
        eyebrow->setTextColor(p.accent);
        card->addView(eyebrow);
        auto* title = new brls::Label();
        title->setText(brls::getStr("lunarnx/ps/connect_title", console_name_));
        title->setFontSize(28);
        title->setTextColor(p.text);
        title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        title->setHeight(60);
        card->addView(title);
        auto* spinner = new brls::ProgressSpinner(brls::ProgressSpinnerSize::LARGE);
        spinner->setWidth(88);
        spinner->setHeight(88);
        card->addView(spinner);
        status_ = new brls::Label();
        status_->setText(brls::getStr("lunarnx/ps/connect_preparing"));
        status_->setFontSize(18);
        status_->setTextColor(p.text);
        status_->setWidth(680);
        status_->setHeight(72);
        status_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        status_->setVerticalAlign(brls::VerticalAlign::CENTER);
        card->addView(status_);
        card->addView(makeMutedLabel("B Cancel", 14));
        root->addView(card);
        diagnosticLog("ui-ps-connect", "createContentView complete");
        return root;
    }

    void onContentAvailable() override {
        diagnosticLog("ui-ps-connect", "onContentAvailable begin");
        auto context = context_;
        auto controller = controller_;
        auto manager = manager_;
        const bool remote = remote_;
        controller_->setLoginPinCallback([this, context](bool incorrect) {
            brls::sync([this, context, incorrect]() {
                if (!context->alive.load()) return;
                requestLoginPin(incorrect);
            });
        });
        controller_->setLaunchCallback([this, context](app::StreamState state,
                                                      const std::string& info) {
            brls::sync([this, context, state, info]() {
                if (!context->alive.load()) return;
                if (status_ && !info.empty()) status_->setText(info);
                if (state == app::StreamState::Streaming) openStream();
            });
        });
        brls::sync([this, context, controller, manager, remote]() {
            if (!context->alive.load() || started_.exchange(true)) return;
            if (context->cancel_requested.load()) {
                context->connect_worker_done = true;
                schedulePsConnectCleanup(context, controller);
                return;
            }
            auto* status = status_;
            bool worker = lunar::platform::startNetworkWorker("ps-connect",
                [context, controller, manager, remote, status]() {
                    bool ok = true;
                    std::string error;
                    ps::PsnAuthErrorKind auth_error_kind = ps::PsnAuthErrorKind::None;
                    if (remote) {
                        bool token_refreshed = false;
                        ok = manager->psnAuth().ensureValidToken({}, &token_refreshed);
                        if (!ok) {
                            error = manager->psnAuth().getAuthError();
                            auth_error_kind = manager->psnAuth().getAuthErrorKind();
                            if (auth_error_kind == ps::PsnAuthErrorKind::SessionExpired) {
                                manager->psnAuth().signOut();
                                std::remove(lunar::get_psn_token_path());
                            }
                        } else if (token_refreshed &&
                                   !manager->psnAuth().saveToken(
                                       lunar::get_psn_token_path())) {
                            ok = false;
                            error = "PSN session refreshed but could not be saved";
                        } else if (!controller->setPsnCredentials(
                                       manager->getPsnAccessToken(),
                                       manager->getPsnAccountId())) {
                            ok = false;
                            error = "PSN credentials could not be applied";
                        }
                    }
                    if (ok && !context->cancel_requested.load()) {
                        ok = controller->startStream();
                        if (!ok) error = controller->lastError();
                    } else if (context->cancel_requested.load()) {
                        ok = false;
                    }
                    context->connect_worker_done = true;
                    diagnosticLog("ui-ps-connect",
                                  "connect worker done ok=%d cancelled=%d",
                                  ok ? 1 : 0,
                                  context->cancel_requested.load() ? 1 : 0);
                    schedulePsConnectCleanup(context, controller);
                    if (!ok) {
                        brls::sync([context, manager, remote, status, auth_error_kind,
                                    error = std::move(error)]() {
                            if (!context->alive.load() ||
                                context->cancel_requested.load()) return;
                            if (status) status->setText(
                                brls::getStr("lunarnx/ps/connect_failed", error));
                            if (remote &&
                                auth_error_kind == ps::PsnAuthErrorKind::SessionExpired) {
                                brls::Application::pushActivity(
                                    new PsnLoginActivity(manager->psnAuth()),
                                    brls::TransitionAnimation::NONE);
                            }
                        });
                    }
                }, 8 * 1024 * 1024);
            if (!worker && status_) status_->setText(brls::getStr("lunarnx/ps/connect_worker_failed"));
        });
    }

private:
    void requestLoginPin(bool incorrect) {
        if (finished_.load()) return;
        auto* input = new brls::InputCell();
        input->init(
            incorrect ? brls::getStr("lunarnx/ps/pin_title_retry") : brls::getStr("lunarnx/ps/pin_title"),
            "",
            [controller = controller_](std::string pin) {
                if (pin.size() == 4) {
                    controller->submitLoginPin(pin);
                    brls::Application::popActivity(brls::TransitionAnimation::NONE);
                }
            },
            brls::getStr("lunarnx/ps/pin_subtitle"),
            brls::getStr("lunarnx/ps/pin_hint"),
            4,
            0);
        brls::Application::pushActivity(
            new brls::Activity(input), brls::TransitionAnimation::NONE);
    }

    void cancel() {
        if (finished_.exchange(true)) return;
        context_->cancel_requested = true;
        diagnosticLog("ui-ps-connect", "cancel requested worker_done=%d",
                      context_->connect_worker_done.load() ? 1 : 0);
        if (status_) status_->setText(brls::getStr("lunarnx/ps/connect_cancelling"));
        controller_->requestCancel();
        schedulePsConnectCleanup(context_, controller_);
    }

    void openStream() {
        if (finished_.exchange(true)) return;
        auto controller = controller_;
        auto open = [controller]() {
            brls::Application::pushActivity(
                new StreamView(controller), brls::TransitionAnimation::NONE);
        };
        if (!brls::Application::popActivity(brls::TransitionAnimation::NONE, open)) open();
    }

    std::shared_ptr<ps::PsStreamController> controller_;
    std::shared_ptr<ps::PsManager> manager_;
    bool remote_ = false;
    std::string console_name_;
    brls::Label* status_ = nullptr;
    std::shared_ptr<PsConnectContext> context_ =
        std::make_shared<PsConnectContext>();
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{false};
};

} // namespace

PsActivity::PsActivity() {
    diagnosticLog("ui-ps", "PsActivity constructor begin");
    try {
        ps_manager_ = std::make_shared<ps::PsManager>();
        ps_manager_->loadCredentials();
        ps_manager_->psnAuth().loadToken(lunar::get_psn_token_path());
        diagnosticLog("ui-ps", "initial state loaded stored_session=%s",
                      ps_manager_->hasStoredPsnSession() ? "true" : "false");
    } catch (...) {
        diagnosticLog("ui-ps", "PsManager creation threw exception");
    }
}

PsActivity::~PsActivity() {
    alive_->store(false);
    wake_generation_->fetch_add(1);
    stopDiscovery();
}

brls::View* PsActivity::createContentView() {
    const auto& p = uiPalette();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            const auto now = std::chrono::steady_clock::now();
            if (now < back_navigation_ready_at_) return true;
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    auto* root = new brls::Box(brls::Axis::ROW);
    root->setBackgroundColor(p.background);

    auto* sidebar = new brls::Box(brls::Axis::COLUMN);
    sidebar->setWidth(232);
    sidebar->setPadding(24, 20, 24, 20);
    sidebar->setBackgroundColor(p.surface);

    auto* wordmark = new brls::Label();
    wordmark->setText("LUNARNX");
    wordmark->setFontSize(26);
    wordmark->setTextColor(p.accent);
    sidebar->addView(wordmark);
    sidebar->addView(makeMutedLabel(brls::getStr("lunarnx/ps/subtitle"), 12));

    local_tab_ = new brls::Button();
    local_tab_->setText(brls::getStr("lunarnx/ps/tab_local"));
    local_tab_->setWidthPercentage(100.0f);
    local_tab_->setMarginTop(24);
    local_tab_->registerClickAction([this](brls::View*) -> bool {
        setConsoleSource(PsConsoleSource::Local);
        return true;
    });
    sidebar->addView(local_tab_);

    remote_tab_ = new brls::Button();
    remote_tab_->setText(brls::getStr("lunarnx/ps/tab_remote"));
    remote_tab_->setWidthPercentage(100.0f);
    remote_tab_->setMarginTop(8);
    remote_tab_->registerClickAction([this](brls::View*) -> bool {
        setConsoleSource(PsConsoleSource::Remote);
        return true;
    });
    sidebar->addView(remote_tab_);

    auto* nav_sep = new brls::Box();
    nav_sep->setHeight(1);
    nav_sep->setBackgroundColor(p.border);
    nav_sep->setMarginTop(18);
    nav_sep->setMarginBottom(18);
    sidebar->addView(nav_sep);

    auto* settings_button = new brls::Button();
    settings_button->setText(brls::getStr("lunarnx/common/settings"));
    settings_button->setWidthPercentage(100.0f);
    styleQuietButton(settings_button);
    settings_button->registerClickAction([](brls::View*) -> bool {
        brls::Application::pushActivity(
            new PsSettingsActivity(loadPsSettings()),
            brls::TransitionAnimation::NONE);
        return true;
    });
    sidebar->addView(settings_button);

    auto* about_button = new brls::Button();
    about_button->setText(brls::getStr("lunarnx/common/about"));
    about_button->setWidthPercentage(100.0f);
    styleQuietButton(about_button);
    about_button->setMarginTop(8);
    about_button->registerClickAction([](brls::View*) -> bool {
        brls::Application::pushActivity(
            new AboutActivity(), brls::TransitionAnimation::NONE);
        return true;
    });
    sidebar->addView(about_button);

    auto* side_spacer = new brls::Box();
    side_spacer->setGrow(1.0f);
    sidebar->addView(side_spacer);

    auto* account_chip = makeUiCard(brls::Axis::ROW);
    account_chip->setHeight(58);
    account_chip->setPadding(7, 10, 7, 10);
    account_chip->setCornerRadius(14);
    account_chip->setAlignItems(brls::AlignItems::CENTER);

    auto* account_mark = new brls::Label();
    account_mark->setWidth(42);
    account_mark->setHeight(42);
    account_mark->setText("PS");
    account_mark->setFontSize(13);
    account_mark->setTextColor(p.accent);
    account_mark->setBackgroundColor(p.accent_soft);
    account_mark->setCornerRadius(21);
    account_mark->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    account_mark->setVerticalAlign(brls::VerticalAlign::CENTER);
    account_chip->addView(account_mark);

    auto* account_copy = new brls::Box(brls::Axis::COLUMN);
    account_copy->setGrow(1.0f);
    account_copy->setPadding(2, 0, 2, 8);
    auto* account_label = new brls::Label();
    account_label->setText(brls::getStr("lunarnx/ps/account_network"));
    account_label->setFontSize(10);
    account_label->setTextColor(p.accent);
    account_copy->addView(account_label);
    account_state_ = new brls::Label();
    account_state_->setFontSize(14);
    account_state_->setTextColor(p.text);
    account_state_->setSingleLine(true);
    account_copy->addView(account_state_);
    account_chip->addView(account_copy);
    sidebar->addView(account_chip);

    account_button_ = new brls::Button();
    styleQuietButton(account_button_);
    account_button_->setWidthPercentage(100.0f);
    account_button_->setMarginTop(8);
    account_button_->registerClickAction([this](brls::View*) -> bool {
        handleAccountAction();
        return true;
    });
    sidebar->addView(account_button_);

    root->addView(sidebar);

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setGrow(1.0f);
    content->setPadding(24, 20, 36, 20);

    remote_actions_ = new brls::Box(brls::Axis::COLUMN);
    auto* remote_header = new brls::Box(brls::Axis::ROW);
    remote_header->setHeight(68);
    remote_header->setAlignItems(brls::AlignItems::CENTER);
    auto* remote_labels = new brls::Box(brls::Axis::COLUMN);
    remote_labels->setGrow(1.0f);
    auto* remote_title = new brls::Label();
    remote_title->setText(brls::getStr("lunarnx/ps/tab_remote"));
    remote_title->setFontSize(25);
    remote_title->setTextColor(p.text);
    remote_labels->addView(remote_title);
    remote_labels->addView(makeMutedLabel(brls::getStr("lunarnx/ps/remote_desc"), 13));
    remote_header->addView(remote_labels);
    psn_button_ = new brls::Button();
    stylePrimaryButton(psn_button_);
    psn_button_->setText(brls::getStr("lunarnx/ps/refresh_psn"));
    psn_button_->registerClickAction([this](brls::View*) -> bool {
        diagnosticLog("ui-ps", "Refresh PSN clicked");
        fetchPsnConsoles();
        return true;
    });
    remote_header->addView(psn_button_);
    remote_actions_->addView(remote_header);
    psn_state_ = makeMutedLabel(brls::getStr("lunarnx/ps/psn_not_checked"), 13);
    psn_state_->setMarginBottom(12);
    remote_actions_->addView(psn_state_);
    content->addView(remote_actions_);

    local_actions_ = new brls::Box(brls::Axis::COLUMN);
    auto* local_header = new brls::Box(brls::Axis::ROW);
    local_header->setHeight(68);
    local_header->setAlignItems(brls::AlignItems::CENTER);
    auto* local_labels = new brls::Box(brls::Axis::COLUMN);
    local_labels->setGrow(1.0f);
    auto* local_title = new brls::Label();
    local_title->setText(brls::getStr("lunarnx/ps/tab_local"));
    local_title->setFontSize(25);
    local_title->setTextColor(p.text);
    local_labels->addView(local_title);
    local_labels->addView(makeMutedLabel(brls::getStr("lunarnx/ps/local_desc"), 13));
    local_header->addView(local_labels);
    lan_button_ = new brls::Button();
    stylePrimaryButton(lan_button_);
    lan_button_->setText(brls::getStr("lunarnx/ps/search_lan"));
    lan_button_->registerClickAction([this](brls::View*) -> bool {
        diagnosticLog("ui-ps", "Search LAN clicked");
        startLanDiscovery();
        return true;
    });
    local_header->addView(lan_button_);
    local_actions_->addView(local_header);
    lan_state_ = makeMutedLabel(brls::getStr("lunarnx/ps/lan_not_searched"), 13);
    lan_state_->setMarginBottom(12);
    local_actions_->addView(lan_state_);

    auto* manual_card = makeUiCard(brls::Axis::ROW);
    manual_card->setHeight(92);
    manual_card->setMarginBottom(18);
    manual_card->setAlignItems(brls::AlignItems::CENTER);
    auto* manual_copy = new brls::Box(brls::Axis::COLUMN);
    manual_copy->setGrow(1.0f);
    auto* manual_title = new brls::Label();
    manual_title->setText(brls::getStr("lunarnx/ps/pair_by_ip"));
    manual_title->setFontSize(16);
    manual_title->setTextColor(p.text);
    manual_copy->addView(manual_title);
    manual_copy->addView(makeMutedLabel(
        brls::getStr("lunarnx/ps/pair_by_ip_desc"), 12));
    manual_card->addView(manual_copy);
    auto add_pair_button = [this, manual_card](const std::string& text, int target) {
        auto* button = new brls::Button();
        styleSecondaryButton(button);
        button->setText(text);
        button->setMarginLeft(8);
        button->registerClickAction([this, target](brls::View*) -> bool {
            ps::PsConsole console;
            console.target = target;
            console.nickname = target >= 1000000 ? "PS5" : "PS4";
            pairConsole(console);
            return true;
        });
        manual_card->addView(button);
    };
    add_pair_button(brls::getStr("lunarnx/ps/pair_ps4"), 900);
    add_pair_button(brls::getStr("lunarnx/ps/pair_ps5"), 1000100);
    local_actions_->addView(manual_card);
    content->addView(local_actions_);

    console_list_ = new brls::Box(brls::Axis::COLUMN);
    console_list_->setMarginBottom(18);
    content->addView(console_list_);

    auto* status_card = makeUiCard(brls::Axis::ROW);
    status_card->setHeight(58);
    status_card->setPadding(12, 18, 12, 18);
    auto* status_mark = new brls::Label();
    status_mark->setText(brls::getStr("lunarnx/common/status"));
    status_mark->setWidth(88);
    status_mark->setFontSize(12);
    status_mark->setTextColor(p.accent);
    status_card->addView(status_mark);
    action_status_ = new brls::Label();
    action_status_->setText("");
    action_status_->setFontSize(14);
    action_status_->setTextColor(p.text_muted);
    action_status_->setGrow(1.0f);
    action_status_->setVerticalAlign(brls::VerticalAlign::CENTER);
    status_card->addView(action_status_);
    content->addView(status_card);
    content->addView(makeHintBar(brls::getStr("lunarnx/common/confirm"),
                                 brls::getStr("lunarnx/common/back")));

    root->addView(content);
    scroll->setContentView(root);
    updateAccountUi();
    hosts_ = ps_manager_ ? ps_manager_->getDiscoveredHosts()
                         : std::vector<ps::PsConsole>{};
    appendMockReplayConsole(hosts_);
    updateSourceUi();
    rebuildConsoleList(hosts_);
    return scroll;
}

void PsActivity::onResume() {
    // A Back press used to close StreamView can remain held for another UI
    // frame. Consume the repeat instead of immediately popping PsActivity and
    // then falling through to the platform page's Exit action.
    back_navigation_ready_at_ = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(750);
    if (!ps_manager_) return;
    ps_manager_->loadCredentials();
    ps_manager_->psnAuth().loadToken(lunar::get_psn_token_path());
    const bool has_session = ps_manager_->hasStoredPsnSession();
    updateAccountUi();
    auto hosts = ps_manager_->getDiscoveredHosts();
    appendMockReplayConsole(hosts);
    rebuildConsoleList(hosts);

    // Existing sessions may not have an online_id stored yet; fetch it once so
    // the account name shows without requiring a re-login.
    if (has_session && ps_manager_->hasPsnToken() &&
        ps_manager_->psnAuth().getOnlineId().empty() && !identity_fetching_->exchange(true)) {
        auto manager = ps_manager_;
        auto alive = alive_;
        lunar::platform::startNetworkWorker("psn-identity",
            [this, manager, alive]() {
                manager->psnAuth().refreshIdentity();
                brls::sync([this, manager, alive]() {
                    if (alive->load()) updateAccountUi();
                });
            });
    }

    if (!resumed_once_) {
        resumed_once_ = true;
        had_psn_session_ = has_session;
    } else {
        if (has_session && !had_psn_session_) fetchPsnConsoles();
        had_psn_session_ = has_session;
    }
}

void PsActivity::startLanDiscovery() {
    if (!ps_manager_) return;
    stopDiscovery();
    if (lan_state_) lan_state_->setText(brls::getStr("lunarnx/ps/lan_searching"));
    auto alive = alive_;
    const bool started = ps_manager_->startDiscovery([this, alive](const std::vector<ps::PsConsole>& hosts) {
        if (!alive->load()) return;
        if (completePendingWake(hosts)) return;
        rebuildConsoleList(hosts);
        if (lan_state_) {
            size_t local_count = 0;
            for (const auto& host : hosts) local_count += host.local.has_value();
            lan_state_->setText(local_count == 0
                ? brls::getStr("lunarnx/ps/lan_none")
                : (local_count == 1
                    ? brls::getStr("lunarnx/ps/lan_found_one", local_count)
                    : brls::getStr("lunarnx/ps/lan_found_many", local_count)));
        }
    });
    if (!started && lan_state_) {
        lan_state_->setText(brls::getStr("lunarnx/ps/lan_search_failed"));
    }
    rebuildConsoleList(ps_manager_->getDiscoveredHosts());
}

void PsActivity::fetchPsnConsoles() {
    if (!ps_manager_) return;
    diagnosticLog("ui-ps", "fetchPsnConsoles stored_session=%s fetching=%s",
                  ps_manager_->hasStoredPsnSession() ? "true" : "false",
                  psn_fetching_->load() ? "true" : "false");
    if (!ps_manager_->hasStoredPsnSession()) {
        if (psn_state_) psn_state_->setText(brls::getStr("lunarnx/ps/psn_sign_in_hint"));
        return;
    }
    if (psn_fetching_->exchange(true)) return;

    if (psn_state_) psn_state_->setText(brls::getStr("lunarnx/ps/psn_refreshing"));
    auto manager = ps_manager_;
    auto fetching = psn_fetching_;
    auto alive = alive_;
    bool started = lunar::platform::startNetworkWorker("psn-device-list",
        [this, alive, manager, fetching]() {
            bool ok = manager->fetchPsnDevices(
                [this, alive](const std::vector<ps::PsConsole>& hosts) {
                    if (!alive->load()) return;
                    rebuildConsoleList(hosts);
                    updateAccountUi();
                    if (psn_state_) psn_state_->setText(brls::getStr("lunarnx/ps/psn_refresh_done"));
                });
            fetching->store(false);
            if (!ok) {
                std::string error = manager->getPsnDeviceError();
                if (error.empty()) error = manager->psnAuth().getAuthError();
                auto error_kind = manager->psnAuth().getAuthErrorKind();
                brls::sync([this, alive, manager, error_kind,
                            error = std::move(error)]() {
                    if (!alive->load()) return;
                    if (error_kind == ps::PsnAuthErrorKind::SessionExpired) {
                        manager->psnAuth().signOut();
                        std::remove(lunar::get_psn_token_path());
                        if (psn_state_) psn_state_->setText(
                            brls::getStr("lunarnx/ps/psn_session_expired", error));
                        brls::Application::pushActivity(
                            new PsnLoginActivity(manager->psnAuth()),
                            brls::TransitionAnimation::NONE);
                    } else {
                        if (psn_state_) psn_state_->setText(
                            brls::getStr("lunarnx/ps/psn_unavailable", error));
                        updateAccountUi();
                    }
                });
            }
        });
    if (!started) {
        fetching->store(false);
        if (psn_state_) psn_state_->setText(brls::getStr("lunarnx/ps/psn_could_not_start"));
    }
}

void PsActivity::stopDiscovery() {
    if (ps_manager_) ps_manager_->stopDiscovery();
}

void PsActivity::setConsoleSource(PsConsoleSource source) {
    if (source_ == source) return;
    source_ = source;
    updateSourceUi();
    rebuildConsoleList(hosts_);
}

void PsActivity::updateSourceUi() {
    const bool local = source_ == PsConsoleSource::Local;
    if (local_tab_) {
        if (local) stylePrimaryButton(local_tab_);
        else styleSecondaryButton(local_tab_);
    }
    if (remote_tab_) {
        if (local) styleSecondaryButton(remote_tab_);
        else stylePrimaryButton(remote_tab_);
    }
    if (local_actions_) local_actions_->setVisibility(
        local ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    if (remote_actions_) remote_actions_->setVisibility(
        local ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
}

void PsActivity::rebuildConsoleList(const std::vector<ps::PsConsole>& hosts) {
    hosts_ = hosts;
    appendMockReplayConsole(hosts_);
    if (!console_list_) return;
    console_list_->clearViews();

    if (hosts.empty()) {
        auto* empty = makeUiCard(brls::Axis::COLUMN);
        empty->setHeight(148);
        empty->setMarginBottom(18);
        empty->setJustifyContent(brls::JustifyContent::CENTER);
        empty->setAlignItems(brls::AlignItems::CENTER);
        auto* title = new brls::Label();
        title->setText(source_ == PsConsoleSource::Local
            ? brls::getStr("lunarnx/ps/empty_local")
            : brls::getStr("lunarnx/ps/empty_remote"));
        title->setFontSize(16);
        title->setTextColor(uiPalette().accent);
        title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        empty->addView(title);
        auto* hint = makeMutedLabel(source_ == PsConsoleSource::Local
            ? brls::getStr("lunarnx/ps/empty_local_hint")
            : brls::getStr("lunarnx/ps/empty_remote_hint"), 13);
        hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        empty->addView(hint);
        console_list_->addView(empty);
        return;
    }

    const auto& p = uiPalette();
    size_t visible_count = 0;
    for (const auto& host : hosts) {
        const bool visible = source_ == PsConsoleSource::Local
            ? host.local.has_value()
            : host.remote.has_value();
        if (!visible) continue;
        visible_count++;
        auto* card = makeUiCard(brls::Axis::ROW);
        card->setHeight(132);
        card->setPadding(16, 18, 16, 18);
        card->setMarginBottom(12);
        card->setAlignItems(brls::AlignItems::CENTER);

        auto* glyph = new ConsoleGlyphView(host.target >= 1000000 ? "PS5" : "PS4",
            host.local.has_value() || (host.remote.has_value() && host.remote->remoteplay_enabled));
        card->addView(glyph);

        auto* info = new brls::Box(brls::Axis::COLUMN);
        info->setGrow(1.0f);
        info->setPadding(6, 20, 6, 20);
        auto* name = new brls::Label();
        name->setText(host.nickname.empty() ? brls::getStr("lunarnx/ps/console_default") : host.nickname);
        name->setFontSize(23);
        name->setTextColor(p.text);
        name->setSingleLine(true);
        name->setVerticalAlign(brls::VerticalAlign::CENTER);
        info->addView(name);

        std::string detail = host.target >= 1000000 ? "PS5" : "PS4";
        if (host.credentials.has_value())
            detail += " · " + brls::getStr("lunarnx/ps/detail_paired");
        if (host.local.has_value()) {
            detail += host.local->state == ps::PsConsoleState::Standby
                ? " · " + brls::getStr("lunarnx/ps/detail_rest")
                : " · " + brls::getStr("lunarnx/ps/detail_ready");
        }
        if (host.remote.has_value()) {
            detail += host.remote->remoteplay_enabled
                ? " · " + brls::getStr("lunarnx/ps/detail_remote")
                : " · " + brls::getStr("lunarnx/ps/detail_remote_disabled");
        }
        auto* meta = makeMutedLabel(detail, 14);
        meta->setHeight(28);
        meta->setVerticalAlign(brls::VerticalAlign::CENTER);
        info->addView(meta);
        auto* description = makeMutedLabel(
            source_ == PsConsoleSource::Local
                ? brls::getStr("lunarnx/ps/local_console_desc")
                : brls::getStr("lunarnx/ps/remote_console_desc"),
            13);
        description->setSingleLine(true);
        info->addView(description);
        card->addView(info);

        const bool paired = host.credentials.has_value();
        const bool local_ready = host.local.has_value() &&
            host.local->state == ps::PsConsoleState::Ready;
        const bool local_standby = host.local.has_value() &&
            host.local->state == ps::PsConsoleState::Standby;
        const bool remote_enabled = host.remote.has_value() &&
            host.remote->remoteplay_enabled;

        auto* action = new brls::Button();
        action->setWidth(190);
        if (source_ == PsConsoleSource::Local && !paired) {
            stylePrimaryButton(action);
            action->setText(brls::getStr("lunarnx/ps/btn_pair"));
            action->registerClickAction([this, host](brls::View*) -> bool {
                pairConsole(host);
                return true;
            });
        } else if (source_ == PsConsoleSource::Local && paired && local_standby) {
            stylePrimaryButton(action);
            action->setText(brls::getStr("lunarnx/ps/btn_wake_connect"));
            action->registerClickAction([this, host](brls::View*) -> bool {
                wakeupConsole(host);
                return true;
            });
        } else if (source_ == PsConsoleSource::Local && paired && local_ready) {
            stylePrimaryButton(action);
            action->setText(brls::getStr("lunarnx/ps/btn_connect"));
            action->registerClickAction([this, host](brls::View*) -> bool {
                diagnosticLog("ui-ps", "Local Connect clicked name=%s",
                              host.nickname.c_str());
                connectToConsole(host);
                return true;
            });
        } else if (source_ == PsConsoleSource::Remote && remote_enabled) {
            stylePrimaryButton(action);
            action->setText(brls::getStr("lunarnx/ps/btn_connect"));
            action->registerClickAction([this, host](brls::View*) -> bool {
                diagnosticLog("ui-ps", "Remote Connect clicked name=%s",
                              host.nickname.c_str());
                connectToConsole(host);
                return true;
            });
        } else if (source_ == PsConsoleSource::Remote && host.remote.has_value() &&
                   !host.remote->remoteplay_enabled) {
            styleSecondaryButton(action);
            action->setText(brls::getStr("lunarnx/ps/btn_how_enable"));
            action->registerClickAction([this](brls::View*) -> bool {
                showRemotePlayHelp();
                return true;
            });
        } else {
            styleSecondaryButton(action);
            action->setText(brls::getStr("lunarnx/ps/btn_unavailable"));
            action->registerClickAction([](brls::View*) -> bool { return true; });
        }
        card->addView(action);
        console_list_->addView(card);
    }

    if (visible_count == 0) {
        auto* empty = makeUiCard(brls::Axis::COLUMN);
        empty->setHeight(148);
        empty->setJustifyContent(brls::JustifyContent::CENTER);
        empty->setAlignItems(brls::AlignItems::CENTER);
        auto* title = new brls::Label();
        title->setText(source_ == PsConsoleSource::Local
            ? brls::getStr("lunarnx/ps/empty_local_none")
            : brls::getStr("lunarnx/ps/empty_remote"));
        title->setFontSize(16);
        title->setTextColor(p.accent);
        title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        empty->addView(title);
        auto* hint = makeMutedLabel(source_ == PsConsoleSource::Local
            ? brls::getStr("lunarnx/ps/empty_local_hint2")
            : brls::getStr("lunarnx/ps/empty_remote_hint2"), 13);
        hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        empty->addView(hint);
        console_list_->addView(empty);
    }
}

bool PsActivity::completePendingWake(const std::vector<ps::PsConsole>& hosts) {
    if (pending_wake_mac_.empty()) return false;
    const auto ready = std::find_if(hosts.begin(), hosts.end(),
        [this](const ps::PsConsole& host) {
            return host.server_mac == pending_wake_mac_ && host.local.has_value() &&
                   host.local->state == ps::PsConsoleState::Ready;
        });
    if (ready == hosts.end()) return false;

    const ps::PsConsole host = *ready;
    pending_wake_mac_.clear();
    wake_generation_->fetch_add(1);
    diagnosticLog("ui-ps", "wake target ready mac=%s ip=%s",
                  host.server_mac.c_str(), host.local->ip.c_str());
    if (action_status_) {
        action_status_->setText(brls::getStr("lunarnx/ps/wake_ready"));
    }
    rebuildConsoleList(hosts);
    connectToConsole(host);
    return true;
}

void PsActivity::pairConsole(const ps::PsConsole& host) {
    int target = host.target >= 1000000 ? 1000100 : 900;
    std::string addr = host.local.has_value() ? host.local->ip : "";
    brls::Application::pushActivity(
        new PsRegistrationActivity(ps_manager_, addr, target,
            host.nickname.empty() ? brls::getStr("lunarnx/ps/console_default") : host.nickname),
        brls::TransitionAnimation::NONE);
}

void PsActivity::wakeupConsole(const ps::PsConsole& host) {
    if (!host.local.has_value() || host.server_mac.empty()) return;
    const auto normalized_mac = ps::normalizeMac(host.server_mac);
    if (!normalized_mac) return;
    const uint64_t generation = wake_generation_->fetch_add(1) + 1;
    pending_wake_mac_ = *normalized_mac;
    if (action_status_) action_status_->setText(brls::getStr("lunarnx/ps/waking", host.nickname));
    auto alive = alive_;
    auto manager = ps_manager_;
    auto wake_generation = wake_generation_;
    const bool started = lunar::platform::startNetworkWorker("ps-wakeup",
        [this, manager, host, alive, wake_generation, generation]() {
            manager->wakeupHost(host.local->ip, host.server_mac,
                host.target >= 1000000,
                [this, alive, host, wake_generation, generation](
                    bool ok, const std::string& error) {
                    brls::sync([this, alive, host, wake_generation, generation,
                                ok, error]() {
                        if (!alive->load() || wake_generation->load() != generation) return;
                        if (!ok) {
                            pending_wake_mac_.clear();
                            wake_generation->fetch_add(1);
                            if (action_status_) action_status_->setText(brls::getStr("lunarnx/ps/wake_failed", error));
                            return;
                        }
                        if (action_status_) action_status_->setText(
                            brls::getStr("lunarnx/ps/wake_waiting"));
                    });
                });
            if (wake_generation->load() != generation) return;
            std::this_thread::sleep_for(std::chrono::seconds(25));
            brls::sync([this, alive, wake_generation, generation]() {
                if (!alive->load() || wake_generation->load() != generation) return;
                pending_wake_mac_.clear();
                wake_generation->fetch_add(1);
                diagnosticLog("ui-ps", "wake target timed out");
                if (action_status_) action_status_->setText(
                    brls::getStr("lunarnx/ps/wake_timeout"));
            });
        });
    if (!started) {
        pending_wake_mac_.clear();
        wake_generation_->fetch_add(1);
        if (action_status_) action_status_->setText(
            brls::getStr("lunarnx/ps/wake_failed", "Could not start wake worker"));
    }
}

void PsActivity::connectToConsole(const ps::PsConsole& host) {
    diagnosticLog("ui-ps", "connectToConsole begin source=%s name=%s local=%s remote=%s",
                  source_ == PsConsoleSource::Local ? "local" : "remote",
                  host.nickname.c_str(),
                  host.local.has_value() ? "true" : "false",
                  host.remote.has_value() ? "true" : "false");
    ps::PsConsole selected = host;
    if (source_ == PsConsoleSource::Local) selected.remote.reset();
    else selected.local.reset();

    const bool remote_enabled = selected.remote.has_value() &&
        selected.remote->remoteplay_enabled && ps_manager_->hasStoredPsnSession();
    if (!selected.credentials.has_value() && !remote_enabled) {
        diagnosticLog("ui-ps", "connect blocked: no usable route credentials=%s remote_enabled=%s",
                      selected.credentials.has_value() ? "true" : "false",
                      remote_enabled ? "true" : "false");
        if (action_status_) action_status_->setText(brls::getStr("lunarnx/ps/no_route"));
        return;
    }
    auto route = ps::PsConsoleResolver::resolve(
        selected, ps_manager_->hasStoredPsnSession());
    if (route.type == ps::ResolvedRouteType::None) {
        diagnosticLog("ui-ps", "connect blocked by resolver error=%s", route.error.c_str());
        if (action_status_) action_status_->setText(route.error);
        return;
    }
    diagnosticLog("ui-ps", "connect route resolved type=%d", static_cast<int>(route.type));
    const bool requested_remote = route.type == ps::ResolvedRouteType::Remote;
    if (action_status_) action_status_->setText(ps::PsConsoleResolver::routeDescription(route));

    const auto settings = loadPsSettings();
    diagnosticLog("ui-ps", "stream profile=%dx%d fps=60 bitrate_kbps=%d codec=%s",
                  settings.width, settings.height, settings.bitrate_kbps,
                  stream::videoCodecName(settings.video_codec));
    auto controller = std::make_shared<ps::PsStreamController>(
        selected, ps_manager_->getPsnAccessToken(), ps_manager_->getPsnAccountId(),
        settings.width, settings.height, 60, settings.bitrate_kbps,
        settings.video_codec);
    const auto stream_settings = loadStreamSettings();
    controller->setRumbleEnabled(stream_settings.vibration_enabled);
    controller->setRumbleStrengthPercent(stream_settings.rumble_strength_percent);
    diagnosticLog("ui-ps", "pushing PsConnectActivity");
    brls::Application::pushActivity(
        new PsConnectActivity(controller, ps_manager_, requested_remote,
            selected.nickname.empty() ? brls::getStr("lunarnx/ps/console_default") : selected.nickname),
        brls::TransitionAnimation::NONE);
    diagnosticLog("ui-ps", "PsConnectActivity push complete");
}

void PsActivity::showRemotePlayHelp() {
    if (action_status_) {
        action_status_->setText(brls::getStr("lunarnx/ps/help_remote_play"));
    }
}

void PsActivity::updateAccountUi() {
    const bool stored = ps_manager_ && ps_manager_->hasStoredPsnSession();
    const bool valid = ps_manager_ && ps_manager_->hasPsnToken();
    std::string online_id = ps_manager_
        ? ps_manager_->psnAuth().getOnlineId() : std::string();
    if (account_state_) {
        account_state_->setText(valid
            ? (online_id.empty()
                ? brls::getStr("lunarnx/ps/signed_in")
                : online_id)
            : stored ? brls::getStr("lunarnx/ps/account_refresh_needed")
                     : brls::getStr("lunarnx/common/not_signed_in"));
    }
    if (account_button_) account_button_->setText(stored
        ? brls::getStr("lunarnx/ps/switch_account")
        : brls::getStr("lunarnx/ps/sign_in"));
    if (psn_state_ && !stored)
        psn_state_->setText(brls::getStr("lunarnx/ps/psn_sign_in_hint"));
}

void PsActivity::handleAccountAction() {
    if (!ps_manager_->hasStoredPsnSession()) {
        signInToPsn();
        return;
    }
    confirmSignOut();
}

void PsActivity::confirmSignOut() {
    auto* dialog = new brls::Dialog(
        brls::getStr("lunarnx/common/sign_out_confirm_message"));
    dialog->addButton(brls::getStr("lunarnx/common/cancel"), []() {});
    dialog->addButton(brls::getStr("lunarnx/common/sign_out"), [this]() {
        ps_manager_->psnAuth().signOut();
        std::remove(lunar::get_psn_token_path());
        had_psn_session_ = false;
        updateAccountUi();
        brls::Application::pushActivity(
            new PsnLoginActivity(ps_manager_->psnAuth()),
            brls::TransitionAnimation::NONE);
    });
    dialog->open();
}

void PsActivity::signInToPsn() {
    auto& auth = ps_manager_->psnAuth();
    if (auth.hasValidToken()) {
        updateAccountUi();
        return;
    }
    auth.loadToken(lunar::get_psn_token_path());
    if (auth.hasValidToken()) {
        updateAccountUi();
        return;
    }
    if (auth.hasStoredSession()) {
        if (psn_state_) psn_state_->setText(brls::getStr("lunarnx/ps/psn_restoring"));
        auto manager = ps_manager_;
        auto alive = alive_;
        bool started = lunar::platform::startNetworkWorker("psn-session-refresh",
            [this, manager, alive]() {
                bool token_refreshed = false;
                bool ok = manager->psnAuth().ensureValidToken({}, &token_refreshed);
                if (ok && token_refreshed) {
                    ok = manager->psnAuth().saveToken(lunar::get_psn_token_path());
                }
                std::string error = manager->psnAuth().getAuthError();
                auto error_kind = manager->psnAuth().getAuthErrorKind();
                brls::sync([this, manager, alive, ok, error_kind,
                            error = std::move(error)]() {
                    if (!alive->load()) return;
                    if (ok) {
                        updateAccountUi();
                        fetchPsnConsoles();
                    } else {
                        if (error_kind == ps::PsnAuthErrorKind::SessionExpired) {
                            manager->psnAuth().signOut();
                            std::remove(lunar::get_psn_token_path());
                            if (psn_state_) psn_state_->setText(
                                brls::getStr("lunarnx/ps/psn_session_expired", error));
                            brls::Application::pushActivity(
                                new PsnLoginActivity(manager->psnAuth()),
                                brls::TransitionAnimation::NONE);
                        } else {
                            if (psn_state_) psn_state_->setText(
                                brls::getStr("lunarnx/ps/psn_unavailable", error));
                            updateAccountUi();
                        }
                    }
                });
            });
        if (!started && psn_state_) psn_state_->setText(brls::getStr("lunarnx/ps/psn_could_not_restore"));
        return;
    }
    brls::Application::pushActivity(
        new PsnLoginActivity(auth), brls::TransitionAnimation::NONE);
}

} // namespace lunar::ui
#endif
