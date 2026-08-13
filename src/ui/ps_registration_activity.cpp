#ifdef __SWITCH__
#include "ps_registration_activity.h"
#include "ui_style.h"
#include "../diagnostics.h"
#include <algorithm>

namespace lunar::ui {

PsRegistrationActivity::PsRegistrationActivity(
    std::shared_ptr<ps::PsManager> manager,
    const std::string& host_addr, int target,
    std::string host_name)
    : manager_(std::move(manager))
    , host_addr_(host_addr)
    , target_(target)
    , host_name_(std::move(host_name)) {}

PsRegistrationActivity::~PsRegistrationActivity() {
    alive_->store(false);
}

brls::View* PsRegistrationActivity::createContentView() {
    const auto& p = uiPalette();

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
    root->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
    root->setPadding(24, 48, 18, 48);
    root->setBackgroundColor(p.background);
    root->setAlignItems(brls::AlignItems::CENTER);
    root->registerAction("Cancel", brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            if (registering_->load()) {
                manager_->cancelRegistration();
            }
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    auto* platform_mark = new brls::Label();
    platform_mark->setWidth(64);
    platform_mark->setHeight(52);
    platform_mark->setText("PS");
    platform_mark->setFontSize(18);
    platform_mark->setTextColor(p.ps_blue);
    platform_mark->setBackgroundColor(p.surface);
    platform_mark->setCornerRadius(26);
    platform_mark->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    platform_mark->setVerticalAlign(brls::VerticalAlign::CENTER);
    root->addView(platform_mark);

    auto* title = new brls::Label();
    title->setText(brls::getStr("lunarnx/ps/reg_title"));
    title->setFontSize(30);
    title->setTextColor(p.text);
    title->setHeight(44);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setVerticalAlign(brls::VerticalAlign::CENTER);
    root->addView(title);

    auto* host_area = new brls::Box(brls::Axis::COLUMN);
    host_area->setWidth(520);
    host_area->setHeight(host_addr_.empty() ? 72 : 38);
    host_area->setAlignItems(brls::AlignItems::CENTER);
    if (host_addr_.empty()) {
        auto* host_input = new brls::InputCell();
        host_input->setWidth(520);
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
        host_area->addView(host_input);
    } else {
        auto* host_label = new brls::Label();
        host_label->setText(host_name_.empty()
            ? host_addr_
            : host_name_ + "  ·  " + host_addr_);
        host_label->setFontSize(14);
        host_label->setTextColor(p.text_muted);
        host_label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        host_area->addView(host_label);
    }
    root->addView(host_area);

    auto* card = makeUiCard(brls::Axis::COLUMN);
    card->setWidth(460);
    card->setHeight(448);
    card->setPadding(24, 24, 20, 24);
    card->setAlignItems(brls::AlignItems::CENTER);

    pin_display_ = new brls::Label();
    pin_display_->setText(brls::getStr("lunarnx/ps/reg_enter_pin"));
    pin_display_->setWidth(412);
    pin_display_->setHeight(64);
    pin_display_->setFontSize(25);
    pin_display_->setTextColor(p.text);
    pin_display_->setBackgroundColor(p.card_muted);
    pin_display_->setCornerRadius(8);
    pin_display_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    pin_display_->setVerticalAlign(brls::VerticalAlign::CENTER);
    card->addView(pin_display_);

    static const char* kRows[] = {"123", "456", "789", " 0 "};
    for (const auto* row : kRows) {
        auto* row_box = new brls::Box(brls::Axis::ROW);
        row_box->setWidth(412);
        row_box->setHeight(54);
        row_box->setJustifyContent(brls::JustifyContent::CENTER);
        row_box->setMarginTop(8);

        for (const char* c = row; *c; c++) {
            if (*c == ' ') {
                auto* spacer = new brls::Box();
                spacer->setDimensions(132, 54);
                row_box->addView(spacer);
                continue;
            }

            char digit = *c;
            auto* btn = new brls::Button();
            btn->setText(std::string(1, digit));
            btn->setWidth(132);
            btn->setHeight(54);
            if (c != row) btn->setMarginLeft(8);
            styleSecondaryButton(btn);
            btn->registerClickAction([this, digit](brls::View*) -> bool {
                onDigit(digit);
                return true;
            });
            row_box->addView(btn);
        }
        card->addView(row_box);
    }

    auto* action_row = new brls::Box(brls::Axis::ROW);
    action_row->setWidth(412);
    action_row->setHeight(54);
    action_row->setJustifyContent(brls::JustifyContent::CENTER);
    action_row->setMarginTop(14);

    auto* back_btn = new brls::Button();
    back_btn->setText(brls::getStr("lunarnx/ps/reg_backspace"));
    back_btn->setWidth(198);
    styleSecondaryButton(back_btn);
    back_btn->registerClickAction([this](brls::View*) -> bool {
        onBackspace();
        return true;
    });
    action_row->addView(back_btn);

    auto* submit_btn = new brls::Button();
    submit_btn->setText(brls::getStr("lunarnx/ps/reg_pair"));
    submit_btn->setWidth(198);
    stylePrimaryButton(submit_btn);
    submit_btn->registerClickAction([this](brls::View*) -> bool {
        onSubmitPin();
        return true;
    });
    submit_btn->setMarginLeft(16);
    action_row->addView(submit_btn);
    card->addView(action_row);

    status_ = new brls::Label();
    status_->setText(brls::getStr("lunarnx/ps/reg_instructions"));
    status_->setFontSize(12);
    status_->setTextColor(p.text_muted);
    status_->setWidth(412);
    status_->setHeight(28);
    status_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    status_->setVerticalAlign(brls::VerticalAlign::CENTER);
    card->addView(status_);
    root->addView(card);

    auto* footer = makeHintBar(brls::getStr("lunarnx/common/confirm"),
                               brls::getStr("lunarnx/common/back"));
    footer->setWidthPercentage(100.0f);
    root->addView(footer);
    return root;
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
    if (pin == 0) return;

    registering_->store(true);
    if (status_) status_->setText(brls::getStr("lunarnx/ps/reg_pairing"));

    auto alive = alive_;
    auto* status_ptr = status_;

    manager_->registerHost(host_addr_, pin, target_,
        [this, alive, status_ptr](ps::RegistrationResult result, const std::string& err) {
            if (!alive->load()) return;

            if (result == ps::RegistrationResult::Success) {
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
