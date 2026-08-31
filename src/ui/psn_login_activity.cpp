#ifdef __SWITCH__
#include "psn_login_activity.h"
#include "qr_code.h"
#include "qr_code_view.h"
#include "i18n.h"
#include "ui_style.h"
#include "../diagnostics.h"
#include "../common.h"
#include "../platform/network_worker.h"

namespace lunar::ui {

PsnLoginActivity::PsnLoginActivity(std::shared_ptr<ps::PsManager> manager)
    : manager_(std::move(manager)) {}

PsnLoginActivity::~PsnLoginActivity() {
    alive_->store(false);
    callback_server_.stop();
}

brls::View* PsnLoginActivity::createContentView() {
    const auto& p = uiPalette();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->registerAction(brls::getStr("lunarnx/common/cancel"),
        brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(18, 34, 30, 18);
    root->setAlignItems(brls::AlignItems::CENTER);

    auto* subtitle = new brls::Label();
    subtitle->setText(brls::getStr("lunarnx/ps/login_subtitle"));
    subtitle->setFontSize(14);
    subtitle->setTextColor(p.text_muted);
    subtitle->setIsWrapping(true);
    subtitle->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    subtitle->setWidth(650);
    subtitle->setMargins(0, 0, 0, 18);
    root->addView(subtitle);

    auto* content = new brls::Box(brls::Axis::ROW);
    content->setWidth(1050);
    content->setJustifyContent(brls::JustifyContent::CENTER);
    content->setAlignItems(brls::AlignItems::CENTER);

    auto* qr_card = makeUiCard(brls::Axis::COLUMN);
    qr_card->setWidth(390);
    qr_card->setPadding(18, 18, 18, 18);
    qr_card->setAlignItems(brls::AlignItems::CENTER);

    auto* scan_title = new brls::Label();
    scan_title->setText(brls::getStr("lunarnx/ps/login_step1"));
    scan_title->setFontSize(17);
    scan_title->setTextColor(p.text);
    scan_title->setMargins(0, 0, 0, 10);
    qr_card->addView(scan_title);

    qr_view_ = new QrCodeView(340.0f);
    qr_card->addView(qr_view_);

    auto* scan_note = new brls::Label();
    scan_note->setText(brls::getStr("lunarnx/ps/login_same_network"));
    scan_note->setFontSize(12);
    scan_note->setTextColor(p.text_muted);
    scan_note->setIsWrapping(true);
    scan_note->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    scan_note->setWidth(340);
    scan_note->setMargins(0, 10, 0, 0);
    qr_card->addView(scan_note);
    content->addView(qr_card);

    auto* guide = new brls::Box(brls::Axis::COLUMN);
    guide->setWidth(550);
    guide->setMarginLeft(28);

    auto add_step = [guide, &p](const std::string& heading,
                                const std::string& detail) {
        auto* step = new brls::Box(brls::Axis::COLUMN);
        step->setWidth(550);
        step->setPadding(4, 0, 12, 0);
        step->setMarginBottom(14);

        auto* step_title = new brls::Label();
        step_title->setText(heading);
        step_title->setFontSize(17);
        step_title->setTextColor(p.text);
        step_title->setWidth(540);
        step_title->setMarginBottom(8);
        step->addView(step_title);

        auto* step_detail = new brls::Label();
        step_detail->setText(detail);
        step_detail->setFontSize(13);
        step_detail->setTextColor(p.text_muted);
        step_detail->setIsWrapping(true);
        step_detail->setWidth(540);
        step->addView(step_detail);
        guide->addView(step);
    };

    add_step(brls::getStr("lunarnx/ps/login_step2"),
             brls::getStr("lunarnx/ps/login_step2_detail"));
    add_step(brls::getStr("lunarnx/ps/login_step3"),
             brls::getStr("lunarnx/ps/login_step3_detail"));
    add_step(brls::getStr("lunarnx/ps/login_step4"),
             brls::getStr("lunarnx/ps/login_step4_detail"));

    status_ = new brls::Label();
    status_->setFontSize(14);
    status_->setTextColor(p.text_muted);
    status_->setIsWrapping(true);
    status_->setWidth(540);
    status_->setMargins(0, 4, 0, 0);
    guide->addView(status_);
    content->addView(guide);
    root->addView(content);

    startPhoneLogin();
    scroll->setContentView(root);
    lunar::diagnosticLog("ui-psn-login", "phone sign-in content ready");
    return makeAppFrame(brls::getStr("lunarnx/ps/login_title"), scroll);
}

void PsnLoginActivity::startPhoneLogin() {
    auto& auth = manager_->psnAuth();
    const std::string login_url = auth.startAuth();
    if (login_url.empty()) {
        if (status_) {
            status_->setText("Could not prepare Sony sign-in: " + auth.getAuthError());
            status_->setTextColor(uiPalette().error);
        }
        return;
    }

    auto alive = alive_;
    const bool started = callback_server_.start(login_url, getResolvedAppLocale(),
        [this, alive](std::string callback_url) {
            if (!alive->load()) return;
            brls::sync([this, alive]() {
                if (!alive->load() || !status_) return;
                status_->setTextColor(uiPalette().text_muted);
                status_->setText("Login address received. Verifying with Sony...");
            });
            exchangeInBackground(std::move(callback_url));
        });

    if (!started) {
        if (qr_view_) qr_view_->clearQrCode();
        if (status_) {
            status_->setText("Could not start phone sign-in: " + callback_server_.getError() +
                             ". Check that the Switch is connected to Wi-Fi.");
            status_->setTextColor(uiPalette().error);
        }
        return;
    }

    const std::string helper_url = callback_server_.getHelperUrl();
    QrCode qr = makeQrCode(helper_url);
    if (qr.empty()) {
        callback_server_.stop();
        if (status_) {
            status_->setText("Could not create the phone sign-in QR code.");
            status_->setTextColor(uiPalette().error);
        }
        return;
    }
    if (qr_view_) qr_view_->setQrCode(std::move(qr));
    if (status_) {
        status_->setTextColor(uiPalette().text_muted);
        status_->setText(brls::getStr("lunarnx/ps/login_waiting"));
    }
    lunar::diagnosticLog("ui-psn-login", "phone helper QR ready url_len=%zu", helper_url.size());
}

void PsnLoginActivity::exchangeInBackground(std::string input) {
    lunar::diagnosticLog("ui-psn-login", "token exchange requested input_len=%zu", input.size());
    auto alive = alive_;
    auto manager = manager_;
    auto* status_ptr = status_;
    bool started = lunar::platform::startNetworkWorker("psn-token-exchange",
        [alive, manager, status_ptr, input = std::move(input)]() {
            auto& auth = manager->psnAuth();
            bool authenticated = auth.submitRedirectUrl(
                input, {}, [alive]() { return !alive->load(); });
            bool saved = authenticated && alive->load() &&
                auth.saveToken(lunar::get_psn_token_path());
            std::string error = authenticated && !saved
                ? "Signed in, but could not save the PSN session"
                : auth.getAuthError();
            brls::sync([alive, status_ptr, authenticated, saved, error = std::move(error)]() {
                if (!alive->load()) return;
                if (authenticated && saved) {
                    brls::Application::popActivity(brls::TransitionAnimation::NONE);
                } else if (status_ptr) {
                    status_ptr->setText("Sign-in failed: " + error +
                        ". Press B and try again to generate a new QR code.");
                    status_ptr->setTextColor(uiPalette().error);
                }
            });
        });
    if (!started && status_) {
        status_->setText("Failed to start the PSN login worker.");
        status_->setTextColor(uiPalette().error);
    }
}

} // namespace lunar::ui
#endif
