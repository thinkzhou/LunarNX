#pragma once
#ifdef __SWITCH__
#include <borealis.hpp>
#include "../app/stream_runtime.h"
#include "../common/operation_generation.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

namespace lunar::ui {

class StreamView : public brls::Activity {
public:
    StreamView(std::shared_ptr<app::IStreamRuntime> runtime);
    ~StreamView();

    brls::View* createContentView() override;
    void onPause() override;
    void onResume() override;

private:
    std::shared_ptr<app::IStreamRuntime> runtime_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> terminal_stop_ =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<common::OperationGeneration> lifecycle_generation_ =
        std::make_shared<common::OperationGeneration>();
    class StreamOverlay* overlay_ = nullptr;
    class PerfOverlay* perf_overlay_ = nullptr;
    brls::Box* content_root_ = nullptr;
    brls::Box* quick_menu_ = nullptr;
    brls::Button* performance_button_ = nullptr;
    brls::Button* resume_button_ = nullptr;
    brls::Button* disconnect_button_ = nullptr;
    brls::Box* confirm_box_ = nullptr;
    brls::Box* stopping_overlay_ = nullptr;
    std::thread update_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_started_{false};
    std::atomic<bool> backgrounded_{false};
    std::atomic<bool> foreground_recovery_running_{false};
    brls::Event<bool>::Subscription focus_subscription_;

    std::chrono::steady_clock::time_point exit_press_time_;
    std::atomic<bool> exit_pending_{false};
    std::atomic<std::chrono::steady_clock::duration::rep> disconnect_arm_ticks_{0};
    bool quick_menu_visible_ = false;
    bool child_activity_visible_ = false;
    bool performance_visible_ = false;
    std::atomic<bool> disconnect_armed_{false};
    float swipe_start_x_ = 0.0f;

    void runLoop();
    void setQuickMenuVisible(bool visible);
    void updateInputOwnership();
    void updatePerformanceVisibility();
    void handleQuickDisconnect();
    void showStoppingOverlay();
    void stopAndReturn();
    void handleWindowFocusChanged(bool focused);
};

} // namespace lunar::ui
#endif
