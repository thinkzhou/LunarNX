#ifdef __SWITCH__
#include "stream_view.h"
#include "stream_overlay.h"
#include "perf_overlay.h"
#include "ui_style.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"
#include "../stream/media_pipeline.h"
#include "../stream/software_video_frame.h"
#include <switch.h>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace lunar::ui {
namespace {

bool isExitComboPressed() {
    PadState pad{};
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);
    u64 buttons = padGetButtons(&pad);
    return (buttons & HidNpadButton_Minus) && (buttons & HidNpadButton_Plus);
}

constexpr float kQuickMenuEdgeWidth = 96.0f;
constexpr float kQuickMenuSwipeDistance = 120.0f;
constexpr auto kQuickDisconnectConfirmWindow = std::chrono::seconds(3);

class SoftwareVideoView : public brls::View {
public:
    ~SoftwareVideoView() override {
        if (nvg_image_ >= 0) {
            nvgDeleteImage(brls::Application::getNVGContext(), nvg_image_);
            nvg_image_ = -1;
        }
    }

    void draw(NVGcontext* vg,
              float x,
              float y,
              float width,
              float height,
              brls::Style style,
              brls::FrameContext* ctx) override {
        (void)style;
        (void)ctx;

        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 255));
        nvgFill(vg);

        auto frame = stream::SoftwareVideoFrameSink::instance().snapshot();
        if (frame.empty()) return;

        if (nvg_image_ < 0 ||
            frame.width != image_width_ ||
            frame.height != image_height_) {
            if (nvg_image_ >= 0) {
                nvgDeleteImage(vg, nvg_image_);
            }
            nvg_image_ = nvgCreateImageRGBA(vg,
                                            frame.width,
                                            frame.height,
                                            0,
                                            frame.rgba.data());
            image_width_ = frame.width;
            image_height_ = frame.height;
            last_generation_ = frame.generation;
        } else if (last_generation_ != frame.generation) {
            nvgUpdateImage(vg, nvg_image_, frame.rgba.data());
            last_generation_ = frame.generation;
        }

        if (nvg_image_ < 0) return;

        const float scale = std::min(width / static_cast<float>(frame.width),
                                     height / static_cast<float>(frame.height));
        const float draw_w = static_cast<float>(frame.width) * scale;
        const float draw_h = static_cast<float>(frame.height) * scale;
        const float draw_x = x + (width - draw_w) * 0.5f;
        const float draw_y = y + (height - draw_h) * 0.5f;
        NVGpaint image = nvgImagePattern(vg,
                                         draw_x,
                                         draw_y,
                                         draw_w,
                                         draw_h,
                                         0.0f,
                                         nvg_image_,
                                         1.0f);
        nvgBeginPath(vg);
        nvgRect(vg, draw_x, draw_y, draw_w, draw_h);
        nvgFillPaint(vg, image);
        nvgFill(vg);
    }

private:
    int nvg_image_ = -1;
    int image_width_ = 0;
    int image_height_ = 0;
    uint64_t last_generation_ = 0;
};

class HardwareVideoView : public brls::View {
public:
    explicit HardwareVideoView(std::shared_ptr<app::IStreamRuntime> runtime)
        : runtime_(std::move(runtime)) {}

    void draw(NVGcontext*, float, float, float, float,
              brls::Style, brls::FrameContext*) override {
        runtime_->presentVideoFrame();
    }

private:
    std::shared_ptr<app::IStreamRuntime> runtime_;
};

}

StreamView::StreamView(std::shared_ptr<app::IStreamRuntime> runtime)
    : runtime_(std::move(runtime)) {}

StreamView::~StreamView() {
    alive_->store(false);
    running_ = false;
    runtime_->setInputSuppressed(false);
    if (update_thread_.joinable()) update_thread_.join();
    auto runtime = runtime_;
    const auto state = runtime_->getState();
    const bool already_reported_failure =
        state == app::StreamState::Disconnected || state == app::StreamState::Error;
    lunar::platform::startNetworkWorker("stop-stream", [runtime, already_reported_failure]() {
        runtime->stopStream(!already_reported_failure);
    });
}

brls::View* StreamView::createContentView() {
    const auto& p = uiPalette();
    auto* root = new brls::Box(brls::Axis::COLUMN);
    content_root_ = root;
    root->setFocusable(true);
    root->setHideHighlight(true);

    // Touch shortcut inspired by XStreaming's in-stream options menu. A
    // leftward swipe beginning at the right edge opens it; a rightward swipe
    // closes it without consuming a controller button used by the game.
    root->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this](brls::PanGestureStatus status, brls::Sound*) {
            if (status.state == brls::GestureState::UNSURE) {
                swipe_start_x_ = status.position.x;
                return;
            }
            if (status.state != brls::GestureState::END) return;
            const float distance = status.position.x - swipe_start_x_;
            if (!quick_menu_visible_ &&
                swipe_start_x_ >= brls::Application::ORIGINAL_WINDOW_WIDTH - kQuickMenuEdgeWidth &&
                distance <= -kQuickMenuSwipeDistance) {
                setQuickMenuVisible(true);
            } else if (quick_menu_visible_ &&
                       distance >= kQuickMenuSwipeDistance) {
                setQuickMenuVisible(false);
            }
        },
        brls::PanAxis::HORIZONTAL));

    // Minus + Plus: stop with double-press confirmation. Keep single Minus as Xbox View.
    auto stop_handler = [this](brls::View*) -> bool {
        if (!isExitComboPressed()) return false;
        runtime_->setInputSuppressed(true);
        auto now = std::chrono::steady_clock::now();
        if (exit_pending_.load() && std::chrono::duration_cast<std::chrono::seconds>(
                now - exit_press_time_).count() < 3) {
            running_ = false;
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        }
        exit_pending_ = true;
        exit_press_time_ = now;
        if (confirm_box_) confirm_box_->setVisibility(brls::Visibility::VISIBLE);
        return true;
    };
    root->registerAction(brls::getStr("lunarnx/stream/stop_action_plus"),
        brls::ControllerButton::BUTTON_START, stop_handler);
    root->registerAction(brls::getStr("lunarnx/stream/stop_action_minus"),
        brls::ControllerButton::BUTTON_BACK, stop_handler);

    if (stream::usesZeroCopyRender(runtime_->getDefaultVideoBackend())) {
        auto* hardware_video = new HardwareVideoView(runtime_);
        hardware_video->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
        hardware_video->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
        hardware_video->setDetachedPosition(0, 0);
        root->addView(hardware_video);
    } else {
        root->setBackgroundColor(nvgRGBA(0, 0, 0, 255));
        auto* software_video = new SoftwareVideoView();
        software_video->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
        software_video->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
        software_video->setDetachedPosition(0, 0);
        root->addView(software_video);
    }

    // Top status bar
    overlay_ = new StreamOverlay(&runtime_->getPerfStats(), runtime_->getStreamPlatform());
    overlay_->detach();
    overlay_->setDetachedPosition(0, 0);
    root->addView(overlay_);

    // Detailed stats overlay (starts hidden)
    perf_overlay_ = new PerfOverlay(&runtime_->getPerfStats(), runtime_->getStreamPlatform());
    perf_overlay_->detach();
    perf_overlay_->setDetachedPosition(10, 76);
    root->addView(perf_overlay_);

    // The menu remains detached and hidden until the button chord is pressed.
    quick_menu_ = new brls::Box(brls::Axis::COLUMN);
    quick_menu_->setWidth(520);
    quick_menu_->setHeight(430);
    quick_menu_->setPadding(28, 32, 24, 32);
    quick_menu_->setBackgroundColor(nvgRGBA(48, 48, 48, 248));
    quick_menu_->setBorderThickness(1);
    quick_menu_->setBorderColor(p.border);
    quick_menu_->setCornerRadius(4);
    quick_menu_->setVisibility(brls::Visibility::GONE);
    quick_menu_->detach();
    quick_menu_->setDetachedPosition(
        (brls::Application::ORIGINAL_WINDOW_WIDTH - 520) / 2,
        (brls::Application::ORIGINAL_WINDOW_HEIGHT - 430) / 2);
    quick_menu_->registerAction(brls::getStr("lunarnx/stream/menu_close"),
        brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            setQuickMenuVisible(false);
            return true;
        });

    auto* menu_title = new brls::Label();
    menu_title->setText(brls::getStr("lunarnx/stream/menu_title"));
    menu_title->setFontSize(25);
    menu_title->setTextColor(p.accent);
    menu_title->setHeight(52);
    quick_menu_->addView(menu_title);

    auto* menu_hint = new brls::Label();
    menu_hint->setText(brls::getStr("lunarnx/stream/menu_hint"));
    menu_hint->setFontSize(13);
    menu_hint->setTextColor(p.text_muted);
    menu_hint->setHeight(58);
    quick_menu_->addView(menu_hint);

    performance_button_ = new brls::Button();
    performance_button_->setHeight(64);
    performance_button_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    performance_button_->setCornerRadius(0);
    performance_button_->setHighlightCornerRadius(3);
    performance_button_->registerClickAction([this](brls::View*) -> bool {
        performance_visible_ = !performance_visible_;
        updatePerformanceVisibility();
        return true;
    });
    quick_menu_->addView(performance_button_);

    auto* platform_button = new brls::Button();
    platform_button->setHeight(64);
    platform_button->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    platform_button->setCornerRadius(0);
    platform_button->setHighlightCornerRadius(3);
    platform_button->setText(brls::getStr(
        runtime_->getStreamPlatform() == app::StreamPlatform::PlayStation
            ? "lunarnx/stream/menu_ps_button"
            : "lunarnx/stream/menu_xbox_button"));
    platform_button->registerClickAction([this](brls::View*) -> bool {
        setQuickMenuVisible(false);
        runtime_->requestPlatformHomeButton();
        return true;
    });
    quick_menu_->addView(platform_button);

    auto* spacer = new brls::Box(brls::Axis::COLUMN);
    spacer->setGrow(1.0f);
    quick_menu_->addView(spacer);

    disconnect_button_ = new brls::Button();
    disconnect_button_->setHeight(64);
    disconnect_button_->setStyle(&brls::BUTTONSTYLE_BORDERED);
    disconnect_button_->setCornerRadius(0);
    disconnect_button_->setHighlightCornerRadius(3);
    disconnect_button_->setTextColor(p.error);
    disconnect_button_->registerClickAction([this](brls::View*) -> bool {
        handleQuickDisconnect();
        return true;
    });
    quick_menu_->addView(disconnect_button_);

    auto* close_hint = new brls::Label();
    close_hint->setText(brls::getStr("lunarnx/stream/menu_close_hint"));
    close_hint->setFontSize(12);
    close_hint->setTextColor(p.text_muted);
    close_hint->setHeight(36);
    close_hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    quick_menu_->addView(close_hint);
    root->addView(quick_menu_);
    updatePerformanceVisibility();

    // Exit confirmation — absolutely positioned at center
    confirm_box_ = new brls::Box(brls::Axis::COLUMN);
    confirm_box_->setBackgroundColor(p.stream_overlay);
    confirm_box_->setBorderThickness(1);
    confirm_box_->setBorderColor(p.accent);
    confirm_box_->setCornerRadius(4);
    confirm_box_->setPadding(24, 20, 24, 20);
    confirm_box_->setWidth(420);
    confirm_box_->setHeight(110);
    confirm_box_->setVisibility(brls::Visibility::GONE);
    confirm_box_->detach();
    int sw = brls::Application::ORIGINAL_WINDOW_WIDTH;
    int sh = brls::Application::ORIGINAL_WINDOW_HEIGHT;
    confirm_box_->setDetachedPosition(
        sw / 2 - 210,  // center X
        sh / 2 - 55    // center Y
    );

    auto* confirm_title = new brls::Label();
    confirm_title->setText(brls::getStr("lunarnx/stream/stop_title"));
    confirm_title->setFontSize(12);
    confirm_title->setTextColor(p.accent);
    confirm_title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    confirm_box_->addView(confirm_title);

    auto* confirm_text = new brls::Label();
    confirm_text->setText(brls::getStr("lunarnx/stream/stop_confirm"));
    confirm_text->setFontSize(18);
    confirm_text->setTextColor(nvgRGBA(255, 255, 255, 255));
    confirm_text->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    confirm_text->setVerticalAlign(brls::VerticalAlign::CENTER);
    confirm_box_->addView(confirm_text);

    auto* confirm_hint = new brls::Label();
    confirm_hint->setText(brls::getStr("lunarnx/stream/stop_cancel_hint"));
    confirm_hint->setFontSize(12);
    confirm_hint->setTextColor(nvgRGBA(140, 140, 140, 255));
    confirm_hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    confirm_box_->addView(confirm_hint);
    root->addView(confirm_box_);

    running_ = true;
    update_thread_ = std::thread(&StreamView::runLoop, this);

    return root;
}

void StreamView::setQuickMenuVisible(bool visible) {
    if (!quick_menu_ || quick_menu_visible_ == visible) return;
    quick_menu_visible_ = visible;
    quick_menu_->setVisibility(
        visible ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    updateInputSuppression();

    disconnect_armed_ = false;
    if (disconnect_button_) {
        disconnect_button_->setText(brls::getStr("lunarnx/stream/menu_disconnect"));
    }
    if (visible && performance_button_) {
        brls::Application::giveFocus(performance_button_);
    } else if (!visible && content_root_) {
        brls::Application::giveFocus(content_root_);
    }
}

void StreamView::updateInputSuppression() {
    runtime_->setInputSuppressed(quick_menu_visible_ || exit_pending_.load());
}

void StreamView::updatePerformanceVisibility() {
    if (overlay_) {
        overlay_->setVisibility(performance_visible_
            ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (!performance_visible_ && perf_overlay_) {
        perf_overlay_->setVisible(false);
    }
    if (performance_button_) {
        performance_button_->setText(brls::getStr(performance_visible_
            ? "lunarnx/stream/menu_hide_performance"
            : "lunarnx/stream/menu_show_performance"));
    }
}

void StreamView::handleQuickDisconnect() {
    const auto now = std::chrono::steady_clock::now();
    if (disconnect_armed_.load()) {
        const auto armed_at = std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(disconnect_arm_ticks_.load()));
        if (now - armed_at < kQuickDisconnectConfirmWindow) {
            running_ = false;
            runtime_->setInputSuppressed(false);
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return;
        }
    }

    disconnect_arm_ticks_ = now.time_since_epoch().count();
    disconnect_armed_ = true;
    if (disconnect_button_) {
        disconnect_button_->setText(
            brls::getStr("lunarnx/stream/menu_disconnect_confirm"));
    }
}

void StreamView::runLoop() {
    using namespace std::chrono;
    auto last_stats = steady_clock::now();
    uint32_t last_frames = 0;
    while (running_) {
        std::this_thread::sleep_for(milliseconds(500));
        const auto now = steady_clock::now();

        auto& p = runtime_->getPerfStats();
        uint32_t frames = p.video_frames.load();
        float sec = duration<float>(now - last_stats).count();
        float fps = (frames - last_frames) / (sec > 0 ? sec : 1.0f);
        last_frames = frames;
        last_stats = now;

        if (disconnect_armed_.load()) {
            const auto disconnect_arm_ticks = disconnect_arm_ticks_.load();
            const auto disconnect_armed_at = steady_clock::time_point(
                steady_clock::duration(disconnect_arm_ticks));
            if (now - disconnect_armed_at >= kQuickDisconnectConfirmWindow) {
                auto alive = alive_;
                brls::sync([alive, this, disconnect_arm_ticks]() {
                    if (!alive->load() || !disconnect_armed_.load() ||
                        disconnect_arm_ticks_.load() != disconnect_arm_ticks) return;
                    disconnect_armed_ = false;
                    if (disconnect_button_) {
                        disconnect_button_->setText(
                            brls::getStr("lunarnx/stream/menu_disconnect"));
                    }
                });
            }
        }

        // Clear exit confirmation after timeout
        if (exit_pending_.load()) {
            auto alive = alive_;
            brls::sync([alive, this]() {
                if (!alive->load()) return;
                auto now = std::chrono::steady_clock::now();
                if (exit_pending_.load() &&
                    std::chrono::duration_cast<std::chrono::seconds>(
                        now - exit_press_time_).count() >= 3) {
                    exit_pending_ = false;
                    updateInputSuppression();
                    if (confirm_box_) confirm_box_->setVisibility(brls::Visibility::GONE);
                }
            });
        }

        // Update overlays
        int w = runtime_->getStreamWidth();
        int h = runtime_->getStreamHeight();
        std::string res = std::to_string(w) + "x" + std::to_string(h);
        std::string video_backend = stream::videoBackendOverlayName(
            runtime_->getDefaultVideoBackend());
        auto alive = alive_;
        brls::sync([alive, this, fps, res, video_backend]() {
            if (!alive->load()) return;
            if (overlay_) overlay_->update(fps, res);
            if (perf_overlay_) perf_overlay_->update(fps, res, video_backend);
        });

        // Detect disconnect/error
        auto state = runtime_->getState();
        if (state == app::StreamState::Disconnected || state == app::StreamState::Error) {
            running_ = false;
            auto alive = alive_;
            brls::sync([alive]() {
                if (!alive->load()) return;
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
            });
            return;
        }
    }
}

} // namespace lunar::ui
#endif
