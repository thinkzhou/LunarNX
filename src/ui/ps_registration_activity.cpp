#ifdef __SWITCH__
#include "ps_registration_activity.h"
#include "i18n.h"
#include "qr_code.h"
#include "qr_code_view.h"
#include "ui_style.h"
#include "../diagnostics.h"
#include <algorithm>

namespace lunar::ui {

PsRegistrationActivity::PsRegistrationActivity(
    std::shared_ptr<ps::PsManager> manager,
    const std::string& host_addr, int target,
    std::string host_name, std::string console_key)
    : manager_(std::move(manager))
    , host_addr_(host_addr)
    , target_(target)
    , host_name_(std::move(host_name))
    , console_key_(std::move(console_key)) {}

PsRegistrationActivity::~PsRegistrationActivity() {
    alive_->store(false);
    phone_pairing_server_.stop();
}

brls::View* PsRegistrationActivity::createContentView() {
    const auto& p = uiPalette();

    auto* scroll = new brls::ScrollingFrame();
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->registerAction("Cancel", brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            if (registering_->load()) {
                manager_->cancelRegistration();
            }
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(24, 80, 24, 80);
    root->setAlignItems(brls::AlignItems::CENTER);

    auto* pairing_card = makeUiCard(brls::Axis::ROW);
    pairing_card->setWidth(920);
    pairing_card->setHeight(470);
    pairing_card->setPadding(24, 28, 24, 28);
    pairing_card->setAlignItems(brls::AlignItems::CENTER);

    auto* details = new brls::Box(brls::Axis::COLUMN);
    details->setWidth(500);
    details->setHeight(420);
    details->setJustifyContent(brls::JustifyContent::CENTER);

    auto* console_type = new brls::Box(brls::Axis::ROW);
    console_type->setWidth(480);
    console_type->setHeight(58);
    console_type->setMarginBottom(12);
    console_type->setAlignItems(brls::AlignItems::CENTER);
    console_type->setBorderThickness(1);
    console_type->setBorderColor(p.border);
    auto* console_type_label = new brls::Label();
    console_type_label->setText("Console Type");
    console_type_label->setFontSize(15);
    console_type_label->setTextColor(p.text);
    console_type_label->setGrow(1.0f);
    console_type_label->setMarginLeft(18);
    console_type->addView(console_type_label);
    auto* console_type_value = new brls::Label();
    console_type_value->setText(target_ >= 1000000 ? "PlayStation 5" : "PlayStation 4");
    console_type_value->setFontSize(14);
    console_type_value->setTextColor(p.accent);
    console_type_value->setMarginRight(18);
    console_type->addView(console_type_value);
    details->addView(console_type);

    if (host_addr_.empty()) {
        auto* host_input = new brls::InputCell();
        host_input->setWidth(480);
        host_input->setHeight(58);
        host_input->setMarginBottom(12);
        host_input->setBackgroundColor(p.surface_alt);
        auto alive = alive_;
        host_input->init(
            brls::getStr("lunarnx/ps/reg_ip_title"),
            "",
            [this, alive](std::string text) {
                if (alive->load()) host_addr_ = std::move(text);
            },
            brls::getStr("lunarnx/ps/reg_ip_subtitle"),
            brls::getStr("lunarnx/ps/reg_ip_example"),
            64,
            0);
        details->addView(host_input);
    } else {
        auto* host_label = new brls::Label();
        host_label->setText(host_name_ + " (" + host_addr_ + ")");
        host_label->setFontSize(14);
        host_label->setTextColor(p.text_muted);
        host_label->setWidth(480);
        host_label->setHeight(58);
        host_label->setMarginBottom(12);
        host_label->setBackgroundColor(p.surface_alt);
        host_label->setBorderThickness(1);
        host_label->setBorderColor(p.border);
        host_label->setMarginLeft(18);
        host_label->setVerticalAlign(brls::VerticalAlign::CENTER);
        details->addView(host_label);
    }

    // PIN display
    pin_display_ = new brls::Label();
    pin_display_->setText(brls::getStr("lunarnx/ps/reg_enter_pin"));
    pin_display_->setFontSize(28);
    pin_display_->setTextColor(p.text);
    pin_display_->setWidth(480);
    pin_display_->setHeight(72);
    pin_display_->setMarginBottom(14);
    pin_display_->setBackgroundColor(p.surface_alt);
    pin_display_->setBorderThickness(1);
    pin_display_->setBorderColor(p.border);
    pin_display_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    pin_display_->setVerticalAlign(brls::VerticalAlign::CENTER);
    details->addView(pin_display_);

    status_ = new brls::Label();
    status_->setText(brls::getStr("lunarnx/ps/reg_instructions"));
    status_->setFontSize(12);
    status_->setTextColor(p.text_muted);
    status_->setWidth(480);
    status_->setHeight(76);
    status_->setIsWrapping(true);
    status_->setVerticalAlign(brls::VerticalAlign::CENTER);
    details->addView(status_);
    pairing_card->addView(details);

    pairing_account_id_ = manager_->getPairingAccountId(console_key_);
    keypad_ = new brls::Box(brls::Axis::COLUMN);
    keypad_->setGrow(1.0f);
    keypad_->setHeight(420);
    keypad_->setAlignItems(brls::AlignItems::CENTER);
    keypad_->setJustifyContent(brls::JustifyContent::CENTER);

    // Number pad (used when an Account ID is already available).
    brls::Button* digit_buttons[10]{};
    static const char* kRows[] = {"123", "456", "789", " 0 "};
    for (const auto* row : kRows) {
        auto* row_box = new brls::Box(brls::Axis::ROW);
        row_box->setHeight(64);
        row_box->setJustifyContent(brls::JustifyContent::CENTER);

        for (const char* c = row; *c; c++) {
            if (*c == ' ') {
                auto* spacer = new brls::Box(brls::Axis::ROW);
                spacer->setDimensions(72, 56);
                spacer->setMargins(4, 4, 4, 4);
                row_box->addView(spacer);
                continue;
            }

            char digit = *c;
            auto* btn = new brls::Button();
            btn->setText(std::string(1, digit));
            btn->setWidth(72);
            btn->setHeight(56);
            btn->setMargins(4, 4, 4, 4);
            styleSecondaryButton(btn);
            btn->registerClickAction([this, digit](brls::View*) -> bool {
                onDigit(digit);
                return true;
            });
            digit_buttons[digit - '0'] = btn;
            row_box->addView(btn);
        }
        keypad_->addView(row_box);
    }

    // Action row
    auto* action_row = new brls::Box(brls::Axis::ROW);
    action_row->setWidth(240);
    action_row->setHeight(64);
    action_row->setJustifyContent(brls::JustifyContent::CENTER);
    action_row->setMarginTop(10);

    auto* back_btn = new brls::Button();
    back_btn->setText(brls::getStr("lunarnx/ps/reg_backspace"));
    back_btn->setWidth(112);
    back_btn->setHeight(54);
    back_btn->setMarginRight(8);
    styleSecondaryButton(back_btn);
    back_btn->registerClickAction([this](brls::View*) -> bool {
        onBackspace();
        return true;
    });
    action_row->addView(back_btn);

    auto* submit_btn = new brls::Button();
    submit_btn->setText(brls::getStr("lunarnx/ps/reg_pair"));
    submit_btn->setWidth(112);
    submit_btn->setHeight(54);
    stylePrimaryButton(submit_btn);
    submit_btn->registerClickAction([this](brls::View*) -> bool {
        onSubmitPin();
        return true;
    });
    action_row->addView(submit_btn);
    keypad_->addView(action_row);
    auto* change_account = new brls::Button();
    change_account->setText(brls::getStr("lunarnx/ps/reg_change_account"));
    change_account->setWidth(240);
    change_account->setHeight(46);
    change_account->setMarginTop(8);
    styleSecondaryButton(change_account);
    change_account->registerClickAction([this](brls::View*) -> bool {
        if (registering_->load()) return true;
        keypad_->setVisibility(brls::Visibility::GONE);
        phone_panel_->setVisibility(brls::Visibility::VISIBLE);
        startPhonePairing();
        return true;
    });
    keypad_->addView(change_account);

    // Borealis normally navigates nested boxes by tree order, which does not
    // preserve the keypad column and can escape a row at its edges. Pin every
    // direction to the visual keypad instead.
    auto route = [](brls::View* from, brls::FocusDirection direction,
                    brls::View* to) {
        from->setCustomNavigationRoute(direction, to);
    };
    const int grid[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            auto* button = digit_buttons[grid[row][column]];
            route(button, brls::FocusDirection::LEFT,
                  digit_buttons[grid[row][column > 0 ? column - 1 : column]]);
            route(button, brls::FocusDirection::RIGHT,
                  digit_buttons[grid[row][column < 2 ? column + 1 : column]]);
            route(button, brls::FocusDirection::UP,
                  digit_buttons[grid[row > 0 ? row - 1 : row][column]]);
            if (row < 2) {
                route(button, brls::FocusDirection::DOWN,
                      digit_buttons[grid[row + 1][column]]);
            }
        }
    }
    route(digit_buttons[7], brls::FocusDirection::DOWN, back_btn);
    route(digit_buttons[8], brls::FocusDirection::DOWN, digit_buttons[0]);
    route(digit_buttons[9], brls::FocusDirection::DOWN, submit_btn);
    route(digit_buttons[0], brls::FocusDirection::UP, digit_buttons[8]);
    route(digit_buttons[0], brls::FocusDirection::LEFT, back_btn);
    route(digit_buttons[0], brls::FocusDirection::RIGHT, submit_btn);
    route(digit_buttons[0], brls::FocusDirection::DOWN, change_account);
    route(back_btn, brls::FocusDirection::UP, digit_buttons[7]);
    route(back_btn, brls::FocusDirection::LEFT, back_btn);
    route(back_btn, brls::FocusDirection::RIGHT, digit_buttons[0]);
    route(back_btn, brls::FocusDirection::DOWN, change_account);
    route(submit_btn, brls::FocusDirection::UP, digit_buttons[9]);
    route(submit_btn, brls::FocusDirection::LEFT, digit_buttons[0]);
    route(submit_btn, brls::FocusDirection::RIGHT, submit_btn);
    route(submit_btn, brls::FocusDirection::DOWN, change_account);
    route(change_account, brls::FocusDirection::UP, digit_buttons[0]);
    route(change_account, brls::FocusDirection::LEFT, change_account);
    route(change_account, brls::FocusDirection::RIGHT, change_account);
    route(change_account, brls::FocusDirection::DOWN, change_account);

    phone_panel_ = new brls::Box(brls::Axis::COLUMN);
    phone_panel_->setGrow(1.0f);
    phone_panel_->setHeight(420);
    phone_panel_->setAlignItems(brls::AlignItems::CENTER);
    phone_panel_->setJustifyContent(brls::JustifyContent::CENTER);
    auto* title = new brls::Label();
    title->setText(brls::getStr("lunarnx/ps/reg_phone_title"));
    title->setFontSize(16);
    title->setTextColor(p.text);
    title->setMarginBottom(10);
    phone_panel_->addView(title);
    phone_qr_view_ = new QrCodeView(250.0f);
    phone_panel_->addView(phone_qr_view_);
    auto* note = new brls::Label();
    note->setText(brls::getStr("lunarnx/ps/reg_phone_note"));
    note->setFontSize(11);
    note->setTextColor(p.text_muted);
    note->setIsWrapping(true);
    note->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    note->setWidth(300);
    note->setMarginTop(8);
    phone_panel_->addView(note);

    const bool needs_phone_account = pairing_account_id_.empty();
    keypad_->setVisibility(needs_phone_account
        ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    phone_panel_->setVisibility(needs_phone_account
        ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    pairing_card->addView(keypad_);
    pairing_card->addView(phone_panel_);
    root->addView(pairing_card);

    scroll->setContentView(root);
    if (needs_phone_account) startPhonePairing();
    return makeAppFrame(brls::getStr("lunarnx/ps/reg_title"), scroll);
}

void PsRegistrationActivity::startPhonePairing() {
    auto alive = alive_;
    const bool started = phone_pairing_server_.start(getResolvedAppLocale(),
        [this, alive](ps::PsPhonePairingInput input) {
            if (!alive->load()) return;
            brls::sync([this, alive, input = std::move(input)]() mutable {
                if (!alive->load()) return;
                pairing_account_id_ = std::move(input.account_id);
                account_id_changed_ = true;
                pin_buffer_ = std::to_string(input.pin);
                if (pin_buffer_.size() < 8) {
                    pin_buffer_.insert(0, 8 - pin_buffer_.size(), '0');
                }
                if (pin_display_) pin_display_->setText("********");
                if (host_addr_.empty()) {
                    if (status_) {
                        status_->setText(brls::getStr("lunarnx/ps/reg_phone_received_need_ip"));
                        status_->setTextColor(uiPalette().text_muted);
                    }
                    return;
                }
                onSubmitPin();
            });
        });
    if (!started) {
        if (phone_qr_view_) phone_qr_view_->clearQrCode();
        if (status_) {
            status_->setText(brls::getStr("lunarnx/ps/reg_phone_start_failed",
                phone_pairing_server_.getError()));
            status_->setTextColor(uiPalette().error);
        }
        return;
    }
    QrCode qr = makeQrCode(phone_pairing_server_.getHelperUrl());
    if (qr.empty()) {
        phone_pairing_server_.stop();
        if (status_) {
            status_->setText(brls::getStr("lunarnx/ps/reg_phone_qr_failed"));
            status_->setTextColor(uiPalette().error);
        }
        return;
    }
    if (phone_qr_view_) phone_qr_view_->setQrCode(std::move(qr));
    if (status_) {
        status_->setText(brls::getStr("lunarnx/ps/reg_phone_waiting"));
    }
}

void PsRegistrationActivity::onDigit(char digit) {
    if (registering_->load()) return;
    if (pin_buffer_.size() >= 8) return;
    pin_buffer_ += digit;
    std::string display(pin_buffer_.size(), '*');
    if (display.size() > 1) display.back() = pin_buffer_.back();
    if (pin_display_) pin_display_->setText(display);
}

void PsRegistrationActivity::onBackspace() {
    if (registering_->load()) return;
    if (pin_buffer_.empty()) return;
    pin_buffer_.pop_back();
    if (pin_display_) {
        if (pin_buffer_.empty()) {
            pin_display_->setText(brls::getStr("lunarnx/ps/reg_enter_pin"));
        } else {
            std::string display(pin_buffer_.size(), '*');
            if (display.size() > 1) display.back() = pin_buffer_.back();
            pin_display_->setText(display);
        }
    }
}

void PsRegistrationActivity::onSubmitPin() {
    if (registering_->load()) return;
    if (host_addr_.empty()) {
        if (status_) status_->setText(brls::getStr("lunarnx/ps/reg_enter_ip_first"));
        return;
    }
    if (pin_buffer_.size() != 8) {
        if (status_) status_->setText(brls::getStr("lunarnx/ps/reg_enter_pin_prompt"));
        return;
    }

    auto pin = static_cast<uint32_t>(std::stoul(pin_buffer_));

    registering_->store(true);
    if (status_) status_->setText(brls::getStr("lunarnx/ps/reg_pairing"));

    // The phone helper is no longer needed after it has delivered the Account
    // ID and PIN. Join it before Chiaki creates its registration worker so the
    // Switch thread limit cannot make chiaki_regist_start() fail spuriously.
    phone_pairing_server_.stop();

    auto alive = alive_;
    auto* status_ptr = status_;

    manager_->registerHost(host_addr_, pin, target_, pairing_account_id_,
        [this, alive, status_ptr](ps::RegistrationResult result, const std::string& err) {
            if (!alive->load()) return;

            if (result == ps::RegistrationResult::Success) {
                if (account_id_changed_ &&
                    !manager_->saveManualPairingAccountId(
                        pairing_account_id_, console_key_)) {
                    diagnosticLog("ui-ps-pair", "paired but could not save local Account ID");
                }
                if (status_ptr) {
                    status_ptr->setText(brls::getStr("lunarnx/ps/reg_success"));
                    status_ptr->setTextColor(uiPalette().success);
                }
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
            } else {
                registering_->store(false);
                if (status_ptr) {
                    status_ptr->setText(brls::getStr("lunarnx/ps/reg_failed", err));
                    status_ptr->setTextColor(uiPalette().error);
                }
            }
        });
}

} // namespace lunar::ui
#endif
