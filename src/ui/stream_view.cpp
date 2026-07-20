#ifdef __SWITCH__
#include "stream_view.h"
#include "stream_overlay.h"
#include "perf_overlay.h"
#include "ui_style.h"
#include "../platform/network_worker.h"
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
    explicit HardwareVideoView(std::shared_ptr<app::StreamController> ctrl)
        : ctrl_(std::move(ctrl)) {}

    void draw(NVGcontext*, float, float, float, float,
              brls::Style, brls::FrameContext*) override {
        ctrl_->presentVideoFrame();
    }

private:
    std::shared_ptr<app::StreamController> ctrl_;
};

}

StreamView::StreamView(std::shared_ptr<app::StreamController> ctrl) : ctrl_(std::move(ctrl)) {}

StreamView::~StreamView() {
    alive_->store(false);
    running_ = false;
    ctrl_->setInputSuppressed(false);
    if (update_thread_.joinable()) update_thread_.join();
    auto ctrl = ctrl_;
    const auto state = ctrl_->getState();
    const bool already_reported_failure =
        state == app::StreamState::Disconnected || state == app::StreamState::Error;
    lunar::platform::startNetworkWorker("stop-stream", [ctrl, already_reported_failure]() {
        ctrl->stopStream(!already_reported_failure);
    });
}

brls::View* StreamView::createContentView() {
    const auto& p = uiPalette();
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setFocusable(true);
    root->setHideHighlight(true);

    // Minus + Plus: stop with double-press confirmation. Keep single Minus as Xbox View.
    auto stop_handler = [this](brls::View*) -> bool {
        if (!isExitComboPressed()) return false;
        ctrl_->setInputSuppressed(true);
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

    // R-stick click: toggle detailed stats only
    root->registerAction(brls::getStr("lunarnx/stream/toggle_stats"),
        brls::ControllerButton::BUTTON_RSB,
        [this](brls::View*) -> bool {
            if (perf_overlay_) perf_overlay_->toggle();
            return true;
        });

    if (stream::usesZeroCopyRender(ctrl_->getDefaultVideoBackend())) {
        auto* hardware_video = new HardwareVideoView(ctrl_);
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
    overlay_ = new StreamOverlay(&ctrl_->getPerfStats());
    overlay_->detach();
    overlay_->setDetachedPosition(0, 0);
    root->addView(overlay_);

    // Detailed stats overlay (R3 toggle, starts hidden)
    perf_overlay_ = new PerfOverlay(&ctrl_->getPerfStats());
    perf_overlay_->detach();
    perf_overlay_->setDetachedPosition(10, 76);
    root->addView(perf_overlay_);

    // Exit confirmation — absolutely positioned at center
    confirm_box_ = new brls::Box(brls::Axis::COLUMN);
    confirm_box_->setBackgroundColor(p.stream_overlay);
    confirm_box_->setBorderThickness(1);
    confirm_box_->setBorderColor(p.accent);
    confirm_box_->setCornerRadius(16);
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

void StreamView::runLoop() {
    using namespace std::chrono;
    auto last_stats = steady_clock::now();
    uint32_t last_frames = 0;
    while (running_) {
        ctrl_->update();
        std::this_thread::sleep_for(milliseconds(500));

        auto now = steady_clock::now();
        auto& p = ctrl_->getPerfStats();
        uint32_t frames = p.video_frames.load();
        float sec = duration<float>(now - last_stats).count();
        float fps = (frames - last_frames) / (sec > 0 ? sec : 1.0f);
        last_frames = frames;
        last_stats = now;

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
                    ctrl_->setInputSuppressed(false);
                    if (confirm_box_) confirm_box_->setVisibility(brls::Visibility::GONE);
                }
            });
        }

        // Update overlays
        int w = ctrl_->getStreamWidth();
        int h = ctrl_->getStreamHeight();
        std::string res = std::to_string(w) + "x" + std::to_string(h);
        std::string video_backend = stream::videoBackendOverlayName(
            ctrl_->getDefaultVideoBackend());
        auto alive = alive_;
        brls::sync([alive, this, fps, res, video_backend]() {
            if (!alive->load()) return;
            if (overlay_) overlay_->update(fps, res);
            if (perf_overlay_) perf_overlay_->update(fps, res, video_backend);
        });

        // Detect disconnect/error
        auto state = ctrl_->getState();
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
