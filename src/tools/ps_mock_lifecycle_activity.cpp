#ifdef __SWITCH__

#include "ps_mock_lifecycle_activity.h"

#include "../ps/ps_stream_controller.h"

#include <switch.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace lunar::tools {
namespace {

constexpr const char* kResultPath =
    "sdmc:/switch/LunarNX/ps_mock_lifecycle.log";

void writeResult(const char* status, const std::string& detail) {
    FILE* output = std::fopen(kResultPath, "a");
    if (!output) return;
    std::fprintf(output, "status=%s %s\n", status, detail.c_str());
    std::fclose(output);
}

ps::PsConnectionPlan mockPlan() {
    ps::PsConnectionPlan plan;
    plan.type = ps::PsConnectionPlanType::LocalPS5;
    plan.target = ps::kPs5RemoteTarget;
    plan.host_addr = "mock-replay";
    return plan;
}

} // namespace

class ps_controller_holder {
public:
    std::mutex mutex;
    std::shared_ptr<ps::PsStreamController> controller;
};

class PsMockLifecycleActivity::ReplayView final : public brls::Box {
public:
    explicit ReplayView(PsMockLifecycleActivity* owner)
        : brls::Box(brls::Axis::COLUMN), owner_(owner) {}

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* context) override {
        brls::Box::draw(vg, x, y, width, height, style, context);
        if (owner_) owner_->present();
    }

private:
    PsMockLifecycleActivity* owner_;
};

PsMockLifecycleActivity::~PsMockLifecycleActivity() {
    running_ = false;
    std::shared_ptr<ps::PsStreamController> controller;
    if (holder_) {
        std::lock_guard<std::mutex> lock(holder_->mutex);
        controller = holder_->controller;
    }
    if (controller) controller->requestCancel();
    if (worker_.joinable()) worker_.join();
}

brls::View* PsMockLifecycleActivity::createContentView() {
    std::remove(kResultPath);
    holder_ = std::make_shared<ps_controller_holder>();
    auto* view = new ReplayView(this);
    view->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
    view->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
    view->setFocusable(true);
    view->setHideHighlight(true);
    worker_ = std::thread([this]() { run(); });
    return view;
}

void PsMockLifecycleActivity::present() {
    std::shared_ptr<ps::PsStreamController> controller;
    {
        std::lock_guard<std::mutex> lock(holder_->mutex);
        controller = holder_->controller;
    }
    if (controller) controller->presentVideoFrame();
}

void PsMockLifecycleActivity::run() {
    // Race cancellation against controller startup before the two full replay
    // rounds. This is the strongest deterministic Switch-side seam available
    // without a live PSN/Chiaki transport.
    {
        auto controller = std::make_shared<ps::PsStreamController>(
            mockPlan(), "", "", 1280, 720, 30, 10000);
        {
            std::lock_guard<std::mutex> lock(holder_->mutex);
            holder_->controller = controller;
        }
        std::thread cancel_thread([controller]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            controller->requestCancel();
        });
        const auto started_at = std::chrono::steady_clock::now();
        const bool started = controller->startStream();
        cancel_thread.join();
        controller->stopStream(false);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        if (elapsed >= std::chrono::seconds(2)) {
            writeResult("FAIL", "cancel_race=timeout started=" +
                std::to_string(started ? 1 : 0));
            running_ = false;
            return;
        }
        writeResult("TRACE", "cancel_race=ok started=" +
            std::to_string(started ? 1 : 0));
        {
            std::lock_guard<std::mutex> lock(holder_->mutex);
            holder_->controller.reset();
        }
    }

    for (int round = 1; round <= 2 && running_; ++round) {
        auto controller = std::make_shared<ps::PsStreamController>(
            mockPlan(), "", "", 1280, 720, 30, 10000);
        {
            std::lock_guard<std::mutex> lock(holder_->mutex);
            holder_->controller = controller;
        }
        writeResult("TRACE", "round=" + std::to_string(round) + " phase=start");
        if (!controller->startStream()) {
            writeResult("FAIL", "round=" + std::to_string(round) +
                " reason=start error=" + controller->lastError());
            running_ = false;
            return;
        }

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(10);
        bool ready = false;
        while (running_ && std::chrono::steady_clock::now() < deadline) {
            controller->update();
            const auto& perf = controller->getPerfStats();
            if (controller->getState() == app::StreamState::Streaming &&
                perf.video_frames.load() >= 30 && perf.audio_frames.load() > 0) {
                ready = true;
                writeResult("TRACE", "round=" + std::to_string(round) +
                    " phase=media video=" + std::to_string(perf.video_frames.load()) +
                    " audio=" + std::to_string(perf.audio_frames.load()));
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!ready) {
            const auto& perf = controller->getPerfStats();
            writeResult("FAIL", "round=" + std::to_string(round) +
                " reason=media_timeout state=" +
                std::to_string(static_cast<int>(controller->getState())) +
                " video=" + std::to_string(perf.video_frames.load()) +
                " audio=" + std::to_string(perf.audio_frames.load()));
            controller->stopStream(false);
            running_ = false;
            return;
        }

        controller->stopStream(false);
        writeResult("TRACE", "round=" + std::to_string(round) + " phase=stopped");
        {
            std::lock_guard<std::mutex> lock(holder_->mutex);
            holder_->controller.reset();
        }
        controller.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (running_) writeResult("PASS", "rounds=2 reconnect=ok cancel_race=ok");
    running_ = false;
}

} // namespace lunar::tools

#endif
