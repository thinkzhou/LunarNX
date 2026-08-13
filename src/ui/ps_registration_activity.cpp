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
    root->setPadding(28, 80, 36, 80);
    root->setAlignItems(brls::AlignItems::CENTER);

    // Title
    auto* title = new brls::Label();
    title->setText(brls::getStr("lunarnx/ps/reg_title"));
    title->setFontSize(24);
    title->setTextColor(p.text);
    title->setMargins(0, 0, 0, 8);
    root->addView(title);

    auto* console_type = new brls::Box(brls::Axis::ROW);
    console_type->setWidth(520);
    console_type->setHeight(64);
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
    root->addView(console_type);

    if (host_addr_.empty()) {
        auto* host_input = new brls::InputCell();
        host_input->setWidth(520);
        host_input->setMargins(0, 0, 0, 18);
        host_input->setHeight(64);
        host_input->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
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
        root->addView(host_input);
    } else {
        auto* host_label = new brls::Label();
        host_label->setText(host_name_ + " (" + host_addr_ + ")");
        host_label->setFontSize(14);
        host_label->setTextColor(p.text_muted);
        host_label->setWidth(520);
        host_label->setHeight(64);
        host_label->setMargins(0, 0, 0, 18);
        host_label->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        host_label->setBorderThickness(1);
        host_label->setBorderColor(p.border);
        host_label->setMarginLeft(18);
        host_label->setVerticalAlign(brls::VerticalAlign::CENTER);
        root->addView(host_label);
    }

    // PIN display
    pin_display_ = new brls::Label();
    pin_display_->setText(brls::getStr("lunarnx/ps/reg_enter_pin"));
    pin_display_->setFontSize(28);
    pin_display_->setTextColor(p.text);
    pin_display_->setWidth(520);
    pin_display_->setHeight(72);
    pin_display_->setMargins(0, 0, 0, 20);
    pin_display_->setBackgroundColor(p.surface_alt);
    pin_display_->setBorderThickness(1);
    pin_display_->setBorderColor(p.border);
    pin_display_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    pin_display_->setVerticalAlign(brls::VerticalAlign::CENTER);
    root->addView(pin_display_);

    // Number pad
    static const char* kRows[] = {"123", "456", "789", " 0 "};
    for (const auto* row : kRows) {
        auto* row_box = new brls::Box(brls::Axis::ROW);
        row_box->setJustifyContent(brls::JustifyContent::CENTER);
        row_box->setMargins(0, 0, 0, 8);

        for (const char* c = row; *c; c++) {
            if (*c == ' ') {
                auto* spacer = new brls::Box(brls::Axis::ROW);
                spacer->setDimensions(60, 50);
                row_box->addView(spacer);
                continue;
            }

            char digit = *c;
            auto* btn = new brls::Button();
            btn->setText(std::string(1, digit));
            btn->setWidth(60);
            btn->setHeight(50);
            styleSecondaryButton(btn);
            btn->registerClickAction([this, digit](brls::View*) -> bool {
                onDigit(digit);
                return true;
            });
            row_box->addView(btn);
        }
        root->addView(row_box);
    }

    // Action row
    auto* action_row = new brls::Box(brls::Axis::ROW);
    action_row->setJustifyContent(brls::JustifyContent::CENTER);
    action_row->setMargins(0, 12, 0, 0);

    auto* back_btn = new brls::Button();
    back_btn->setText(brls::getStr("lunarnx/ps/reg_backspace"));
    back_btn->setWidth(120);
    styleQuietButton(back_btn);
    back_btn->registerClickAction([this](brls::View*) -> bool {
        onBackspace();
        return true;
    });
    action_row->addView(back_btn);

    auto* submit_btn = new brls::Button();
    submit_btn->setText(brls::getStr("lunarnx/ps/reg_pair"));
    submit_btn->setWidth(120);
    stylePrimaryButton(submit_btn);
    submit_btn->registerClickAction([this](brls::View*) -> bool {
        onSubmitPin();
        return true;
    });
    submit_btn->setMargins(20, 0, 0, 0);
    action_row->addView(submit_btn);
    root->addView(action_row);

    // Status
    status_ = new brls::Label();
    status_->setText(brls::getStr("lunarnx/ps/reg_instructions"));
    status_->setFontSize(12);
    status_->setTextColor(p.text_muted);
    status_->setMargins(0, 20, 0, 0);
    root->addView(status_);

    scroll->setContentView(root);
    return makeAppFrame(brls::getStr("lunarnx/ps/reg_title"), scroll);
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
