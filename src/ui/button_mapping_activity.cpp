#ifdef __SWITCH__
#include "button_mapping_activity.h"

#include "ui_style.h"

#include <algorithm>
#include <array>
#include <functional>
#include <string>

namespace lunar::ui {
namespace {

constexpr int kCaptureReleaseFrames = 6;
constexpr uint64_t kSupportedButtons =
    HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y |
    HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR |
    HidNpadButton_StickL | HidNpadButton_StickR |
    HidNpadButton_Minus | HidNpadButton_Plus |
    HidNpadButton_Up | HidNpadButton_Down |
    HidNpadButton_Left | HidNpadButton_Right |
    input::kButtonMappingCapture;

constexpr const char* kXboxLabels[] = {
    "lunarnx/button_mapping/xbox_a", "lunarnx/button_mapping/xbox_b",
    "lunarnx/button_mapping/xbox_x", "lunarnx/button_mapping/xbox_y",
    "lunarnx/button_mapping/dpad_up", "lunarnx/button_mapping/dpad_down",
    "lunarnx/button_mapping/dpad_left", "lunarnx/button_mapping/dpad_right",
    "lunarnx/button_mapping/lb", "lunarnx/button_mapping/rb",
    "lunarnx/button_mapping/lt", "lunarnx/button_mapping/rt",
    "lunarnx/button_mapping/l3", "lunarnx/button_mapping/r3",
    "lunarnx/button_mapping/view", "lunarnx/button_mapping/menu",
    "lunarnx/button_mapping/guide",
};

constexpr const char* kPlayStationLabels[] = {
    "lunarnx/button_mapping/cross", "lunarnx/button_mapping/circle",
    "lunarnx/button_mapping/square", "lunarnx/button_mapping/triangle",
    "lunarnx/button_mapping/dpad_up", "lunarnx/button_mapping/dpad_down",
    "lunarnx/button_mapping/dpad_left", "lunarnx/button_mapping/dpad_right",
    "lunarnx/button_mapping/l1", "lunarnx/button_mapping/r1",
    "lunarnx/button_mapping/l2", "lunarnx/button_mapping/r2",
    "lunarnx/button_mapping/l3", "lunarnx/button_mapping/r3",
    "lunarnx/button_mapping/share", "lunarnx/button_mapping/options",
    "lunarnx/button_mapping/ps", "lunarnx/button_mapping/touchpad",
};

class CapturePollingBox : public brls::Box {
public:
    explicit CapturePollingBox(std::function<void()> poll)
        : brls::Box(brls::Axis::COLUMN), poll_(std::move(poll)) {}

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        poll_();
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

private:
    std::function<void()> poll_;
};

} // namespace

ButtonMappingActivity::ButtonMappingActivity(input::ButtonMappingProfile profile)
    : mapping_(input::loadButtonMapping(profile)), profile_(profile) {
    input::acquireCaptureButtonInput();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&capture_pad_);
}

ButtonMappingActivity::~ButtonMappingActivity() {
    input::releaseCaptureButtonInput();
}

brls::View* ButtonMappingActivity::createContentView() {
    const auto& p = uiPalette();
    auto* root = new CapturePollingBox([this]() { pollCaptureInput(); });
    root->setBackgroundColor(p.background);
    root->setFocusable(true);
    root->setHideHighlight(true);
    root->registerAction(brls::getStr("lunarnx/settings/back_action"),
        brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            if (capturing_) return true;
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    mapping_content_ = new brls::Box(brls::Axis::COLUMN);
    mapping_content_->setGrow(1.0f);
    root->addView(mapping_content_);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    mapping_content_->addView(scroll);
    auto* footer = makeHintBar(brls::getStr("lunarnx/common/confirm"),
                               brls::getStr("lunarnx/common/back"));
    footer->setBackgroundColor(p.surface);
    mapping_content_->addView(footer);

    auto* stage = new brls::Box(brls::Axis::COLUMN);
    stage->setPadding(26, 0, 28, 0);
    stage->setAlignItems(brls::AlignItems::CENTER);
    scroll->setContentView(stage);

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setWidth(860);
    stage->addView(content);

    auto* title = new brls::Label();
    title->setText(brls::getStr(
        profile_ == input::ButtonMappingProfile::PlayStation
            ? "lunarnx/button_mapping/ps_title"
            : "lunarnx/button_mapping/xbox_title"));
    title->setFontSize(30);
    title->setTextColor(p.text);
    title->setHeight(52);
    content->addView(title);
    auto* detail = makeMutedLabel(
        brls::getStr("lunarnx/button_mapping/detail"), 14);
    detail->setHeight(42);
    detail->setIsWrapping(true);
    content->addView(detail);

    auto* focus_wash = makeUiCard();
    focus_wash->setBackgroundColor(p.accent_soft);
    focus_wash->setBorderColor(p.accent);
    focus_wash->setPadding(18, 68, 18, 68);
    focus_wash->setMarginTop(14);

    auto* card = makeUiCard();
    card->setPadding(4, 8, 4, 8);
    const size_t row_count = profile_ == input::ButtonMappingProfile::PlayStation
        ? input::kRemoteButtonCount
        : static_cast<size_t>(input::RemoteButton::Touchpad);
    const char* const* labels = profile_ == input::ButtonMappingProfile::PlayStation
        ? kPlayStationLabels
        : kXboxLabels;
    for (size_t i = 0; i < row_count; ++i) {
        auto* row = new brls::DetailCell();
        row->setHeight(54);
        row->setText(brls::getStr(labels[i]));
        row->setFocusable(true);
        row->registerClickAction([this, i](brls::View*) -> bool {
            enterCapture(i);
            return true;
        });
        card->addView(row);
        rows_.push_back(row);
    }
    focus_wash->addView(card);
    content->addView(focus_wash);

    auto* reset = new brls::Button();
    reset->setWidth(220);
    reset->setText(brls::getStr("lunarnx/button_mapping/reset_all"));
    styleSecondaryButton(reset);
    reset->setMarginTop(24);
    reset->registerClickAction([this](brls::View*) -> bool {
        mapping_ = input::defaultButtonMapping(profile_);
        if (input::saveButtonMapping(profile_, mapping_)) {
            refreshRows();
            brls::Application::notify(
                brls::getStr("lunarnx/button_mapping/reset_done"));
        }
        return true;
    });
    content->addView(reset);

    capture_content_ = new brls::Box(brls::Axis::COLUMN);
    capture_content_->setGrow(1.0f);
    capture_content_->setPadding(180, 120, 180, 120);
    capture_content_->setAlignItems(brls::AlignItems::CENTER);
    capture_content_->setJustifyContent(brls::JustifyContent::CENTER);
    capture_content_->setFocusable(true);
    capture_content_->setHideHighlight(true);
    capture_content_->setVisibility(brls::Visibility::GONE);
    root->addView(capture_content_);

    auto* capture_title = new brls::Label();
    capture_title->setText(brls::getStr("lunarnx/button_mapping/capture_title"));
    capture_title->setFontSize(30);
    capture_title->setTextColor(p.text);
    capture_title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    capture_title->setMarginBottom(28);
    capture_content_->addView(capture_title);

    capture_status_ = new brls::Label();
    capture_status_->setFontSize(22);
    capture_status_->setTextColor(p.accent);
    capture_status_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    capture_content_->addView(capture_status_);

    auto* capture_hint = makeMutedLabel(
        brls::getStr("lunarnx/button_mapping/capture_hint"), 14);
    capture_hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    capture_hint->setMarginTop(20);
    capture_content_->addView(capture_hint);

    refreshRows();
    return root;
}

void ButtonMappingActivity::enterCapture(size_t index) {
    capture_index_ = index;
    peak_buttons_ = 0;
    saw_button_ = false;
    release_frames_ = 0;
    capturing_ = true;
    waiting_for_release_ = true;
    capture_status_->setText(
        brls::getStr("lunarnx/button_mapping/wait_release"));
    mapping_content_->setVisibility(brls::Visibility::GONE);
    capture_content_->setVisibility(brls::Visibility::VISIBLE);
    brls::Application::giveFocus(capture_content_);
}

void ButtonMappingActivity::pollCaptureInput() {
    if (!capturing_) return;
    padUpdate(&capture_pad_);
    uint64_t buttons = padGetButtons(&capture_pad_) & kSupportedButtons;
    if (input::isCaptureButtonPressed()) {
        buttons |= input::kButtonMappingCapture;
    }
    if (waiting_for_release_) {
        if (buttons == 0) {
            waiting_for_release_ = false;
            capture_status_->setText(
                brls::getStr("lunarnx/button_mapping/press_buttons"));
        }
        return;
    }
    if (buttons != 0) {
        saw_button_ = true;
        release_frames_ = 0;
        peak_buttons_ |= buttons;
        capture_status_->setText(input::formatHidButtonMask(peak_buttons_));
        return;
    }
    if (saw_button_ && ++release_frames_ >= kCaptureReleaseFrames) {
        finishCapture();
    }
}

void ButtonMappingActivity::finishCapture() {
    const uint64_t quick_menu = HidNpadButton_Minus | HidNpadButton_Plus;
    if ((peak_buttons_ & quick_menu) == quick_menu) {
        peak_buttons_ = 0;
        saw_button_ = false;
        release_frames_ = 0;
        capture_status_->setText(
            brls::getStr("lunarnx/button_mapping/reserved_chord"));
        return;
    }
    mapping_[capture_index_] = peak_buttons_;
    input::saveButtonMapping(profile_, mapping_);
    capturing_ = false;
    capture_content_->setVisibility(brls::Visibility::GONE);
    mapping_content_->setVisibility(brls::Visibility::VISIBLE);
    refreshRows();
    if (capture_index_ < rows_.size()) {
        brls::Application::giveFocus(rows_[capture_index_]);
    }
}

bool ButtonMappingActivity::hasConflict(size_t index) const {
    if (mapping_[index] == 0) return false;
    return std::count(mapping_.begin(), mapping_.end(), mapping_[index]) > 1;
}

void ButtonMappingActivity::refreshRows() {
    const auto& p = uiPalette();
    for (size_t i = 0; i < rows_.size(); ++i) {
        std::string detail = mapping_[i] == 0
            ? brls::getStr("lunarnx/button_mapping/not_mapped")
            : input::formatHidButtonMask(mapping_[i]);
        if (hasConflict(i)) {
            detail += "  ";
            detail += brls::getStr("lunarnx/button_mapping/conflict");
            rows_[i]->setDetailTextColor(p.error);
            rows_[i]->setBackgroundColor(nvgRGBA(
                static_cast<unsigned char>(p.error.r * 255.0f),
                static_cast<unsigned char>(p.error.g * 255.0f),
                static_cast<unsigned char>(p.error.b * 255.0f),
                30));
        } else {
            rows_[i]->setDetailTextColor(p.text_muted);
            rows_[i]->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        }
        rows_[i]->setDetailText(detail);
    }
}

} // namespace lunar::ui
#endif
