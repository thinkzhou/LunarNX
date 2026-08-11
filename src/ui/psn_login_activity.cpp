#ifdef __SWITCH__
#include "psn_login_activity.h"
#include "qr_code.h"
#include "qr_code_view.h"
#include "ui_style.h"
#include "../diagnostics.h"
#include "../common.h"
#include "../platform/network_worker.h"
#include <borealis/views/cells/cell_input.hpp>
#include <cstdio>

namespace lunar::ui {

PsnLoginActivity::PsnLoginActivity(ps::PsnAuthManager& auth) : auth_(auth) {}
PsnLoginActivity::~PsnLoginActivity() { alive_->store(false); }

brls::View* PsnLoginActivity::createContentView() {
    const auto& p = uiPalette();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->registerAction("Cancel", brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(30, 40, 30, 20);
    root->setAlignItems(brls::AlignItems::CENTER);

    auto* title = new brls::Label();
    title->setText("Sign in to PlayStation Network");
    title->setFontSize(20);
    title->setTextColor(p.text);
    title->setMargins(0, 0, 0, 24);
    root->addView(title);

    // ── Primary: Switch browser ────────────────────────────────────────

    auto* browser_card = makeUiCard(brls::Axis::COLUMN);
    browser_card->setWidth(540);
    browser_card->setPadding(18, 20, 18, 20);
    browser_card->setMarginBottom(16);

    auto* browser_title = new brls::Label();
    browser_title->setText("Sign in with Switch Browser");
    browser_title->setFontSize(17);
    browser_title->setTextColor(p.text);
    browser_card->addView(browser_title);

    auto* browser_hint = new brls::Label();
    browser_hint->setText("The Switch system browser will open Sony's sign-in page. "
                          "After you sign in, the app captures your login automatically. "
                          "No phone or manual code entry needed.");
    browser_hint->setFontSize(13);
    browser_hint->setTextColor(p.text_muted);
    browser_hint->setIsWrapping(true);
    browser_hint->setWidth(500);
    browser_hint->setMargins(0, 6, 0, 14);
    browser_card->addView(browser_hint);

    auto* browser_btn = new brls::Button();
    browser_btn->setText("Open Browser");
    browser_btn->setWidth(280);
    stylePrimaryButton(browser_btn);
    browser_btn->registerClickAction([this](brls::View*) -> bool {
        openBrowser();
        return true;
    });
    browser_card->addView(browser_btn);
    root->addView(browser_card);

    // ── Secondary: manual code entry ───────────────────────────────────

    auto* manual_section = new brls::Box(brls::Axis::COLUMN);
    manual_section->setWidth(540);

    auto toggle_expand = [this]() {
        manual_expanded_ = !manual_expanded_;
        if (manual_body_) {
            manual_body_->setVisibility(manual_expanded_
                ? brls::Visibility::VISIBLE
                : brls::Visibility::GONE);
        }
        if (manual_toggle_) {
            manual_toggle_->setText(manual_expanded_ ? "Hide manual entry ▲" : "Manual entry (QR code / paste URL) ▼");
        }
    };

    manual_toggle_ = new brls::Button();
    manual_toggle_->setText("Manual entry (QR code / paste URL) ▼");
    manual_toggle_->setWidth(420);
    manual_toggle_->setMargins(0, 0, 0, 12);
    styleSecondaryButton(manual_toggle_);
    manual_toggle_->registerClickAction([toggle_expand](brls::View*) -> bool {
        toggle_expand();
        return true;
    });
    manual_section->addView(manual_toggle_);

    manual_body_ = new brls::Box(brls::Axis::COLUMN);
    manual_body_->setVisibility(brls::Visibility::GONE);

    auto* manual_card = makeUiCard(brls::Axis::COLUMN);
    manual_card->setWidth(540);
    manual_card->setPadding(16, 16, 16, 16);
    manual_card->setMarginBottom(14);

    auto* scan_hint = new brls::Label();
    scan_hint->setText("1. Scan the QR code and sign in on your phone.\n"
                       "2. Copy the final redirect URL, even if the page does not load.\n"
                       "3. Select the input field below and paste the URL or code.");
    scan_hint->setFontSize(13);
    scan_hint->setTextColor(p.text_muted);
    scan_hint->setIsWrapping(true);
    scan_hint->setWidth(500);
    scan_hint->setMargins(0, 0, 0, 14);
    manual_card->addView(scan_hint);

    std::string url = auth_.startAuth();
    lunar::diagnosticLog("ui-psn-login", "auth URL length=%zu", url.size());

    auto* qr_view = new QrCodeView(200.0f);
    qr_view->setMargins(0, 0, 0, 14);
    if (!url.empty()) {
        lunar::diagnosticLog("ui-psn-login", "QR encode begin");
        QrCode qr = makeQrCode(url);
        lunar::diagnosticLog("ui-psn-login", "QR encode done size=%d", qr.size);
        qr_view->setQrCode(std::move(qr));
        lunar::diagnosticLog("ui-psn-login", "QR view ready");
    }
    manual_card->addView(qr_view);

    auto alive = alive_;
    auto* callback_input = new brls::InputCell();
    callback_input->setWidth(500);
    callback_input->init(
        "Enter Code / URL",
        "",
        [this, alive](std::string text) {
            lunar::diagnosticLog("ui-psn-login", "manual callback length=%zu", text.size());
            if (!alive->load() || text.empty()) {
                if (status_) status_->setText("No callback code was entered.");
                return;
            }
            if (status_) {
                status_->setTextColor(uiPalette().text_muted);
                status_->setText("Code received. Verifying...");
            }
            exchangeInBackground(std::move(text));
        },
        "Select to paste callback",
        "Paste the full redirect URL or the value after code=",
        512,
        0);
    callback_input->setMargins(0, 0, 0, 10);
    manual_card->addView(callback_input);

    auto* import_btn = new brls::Button();
    styleSecondaryButton(import_btn);
    import_btn->setText("Import from SD Card");
    import_btn->setWidth(260);
    import_btn->registerClickAction([this](brls::View*) -> bool {
        importCallbackFile();
        return true;
    });
    manual_card->addView(import_btn);

    manual_body_->addView(manual_card);
    manual_section->addView(manual_body_);
    root->addView(manual_section);

    // ── Status ─────────────────────────────────────────────────────────

    status_ = new brls::Label();
    status_->setFontSize(13);
    status_->setTextColor(p.text_muted);
    status_->setIsWrapping(true);
    status_->setWidth(500);
    status_->setMargins(0, 14, 0, 0);
    status_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    root->addView(status_);

    if (url.empty() && status_) status_->setText("Failed: " + auth_.getAuthError());

    scroll->setContentView(root);
    lunar::diagnosticLog("ui-psn-login", "content ready");
    return scroll;
}

void PsnLoginActivity::openBrowser() {
    if (status_) {
        status_->setTextColor(uiPalette().text_muted);
        status_->setText("Opening Switch browser...");
    }

    // webConfigShow blocks until the browser applet exits. During that time
    // the Switch display is fully occupied by the system browser and the
    // Borealis UI is hidden. The call must stay on the main thread because
    // it communicates with the applet host that was initialised there.
    std::string code;
    if (!auth_.openWebApplet(code)) {
        if (status_) {
            status_->setText("Browser login failed: " + auth_.getAuthError());
            status_->setTextColor(uiPalette().error);
        }
        return;
    }

    if (status_) {
        status_->setTextColor(uiPalette().text_muted);
        status_->setText("Signed in. Verifying...");
    }

    // Token exchange involves network requests — dispatch to a worker.
    exchangeInBackground(std::move(code));
}

void PsnLoginActivity::importCallbackFile() {
    const char* path = lunar::get_psn_callback_import_path();
    lunar::diagnosticLog("ui-psn-login", "callback import requested path=%s", path);

    FILE* file = std::fopen(path, "rb");
    if (!file) {
        if (status_) {
            status_->setText("Import file not found:\n"
                             "sdcard/switch/LunarNX/psn_callback.txt");
            status_->setTextColor(uiPalette().error);
        }
        return;
    }

    std::string input;
    char buffer[512];
    while (input.size() < 4096) {
        const size_t count = std::fread(buffer, 1, sizeof(buffer), file);
        input.append(buffer, count);
        if (count < sizeof(buffer)) break;
    }
    std::fclose(file);

    while (!input.empty() &&
           (input.back() == '\n' || input.back() == '\r' || input.back() == ' ' || input.back() == '\t')) {
        input.pop_back();
    }
    size_t first = 0;
    while (first < input.size() &&
           (input[first] == '\n' || input[first] == '\r' || input[first] == ' ' || input[first] == '\t')) {
        ++first;
    }
    if (first > 0) input.erase(0, first);

    if (input.empty()) {
        if (status_) {
            status_->setText("psn_callback.txt is empty.");
            status_->setTextColor(uiPalette().error);
        }
        return;
    }

    std::remove(path);
    lunar::diagnosticLog("ui-psn-login", "callback imported length=%zu file_removed=true", input.size());
    if (status_) {
        status_->setTextColor(uiPalette().text_muted);
        status_->setText("Callback imported. Verifying...");
    }
    exchangeInBackground(std::move(input));
}

void PsnLoginActivity::exchangeInBackground(std::string input) {
    lunar::diagnosticLog("ui-psn-login", "token exchange requested input_len=%zu", input.size());
    auto alive = alive_;
    auto& auth = auth_;
    auto* status_ptr = status_;
    bool started = lunar::platform::startNetworkWorker("psn-token-exchange",
        [alive, &auth, status_ptr, input = std::move(input)]() {
            bool authenticated = auth.submitRedirectUrl(input, {});
            bool saved = authenticated && auth.saveToken(lunar::get_psn_token_path());
            std::string error = authenticated && !saved
                ? "Signed in, but could not save the PSN session"
                : auth.getAuthError();
            brls::sync([alive, status_ptr, authenticated, saved, error = std::move(error)]() {
                if (!alive->load()) return;
                if (authenticated && saved) {
                    brls::Application::popActivity(brls::TransitionAnimation::NONE);
                } else if (status_ptr) {
                    status_ptr->setText("Failed: " + error);
                    status_ptr->setTextColor(uiPalette().error);
                }
            });
        });
    if (!started && status_) {
        status_->setText("Failed to start PSN login worker");
        status_->setTextColor(uiPalette().error);
    }
}

} // namespace lunar::ui
#endif
