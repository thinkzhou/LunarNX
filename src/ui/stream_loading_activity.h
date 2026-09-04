#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>

#include "../app/stream_controller.h"
#include "connection_cancel_state.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace lunar::ui {

enum class StreamLaunchTarget {
    HomeConsole,
    CloudTitle,
};

enum class StreamLaunchResult {
    Started,
    Failed,
    Cancelled,
};

struct StreamLaunchRequest {
    StreamLaunchTarget target = StreamLaunchTarget::HomeConsole;
    std::string target_id;
    std::string display_name;
    int width = 1280;
    int height = 720;
    // Zero keeps direct callers on StreamController's resolution default.
    int bitrate_kbps = 0;
    stream::MediaPipelineOptions options;
};

class StreamLoadingActivity : public brls::Activity {
public:
    using CompletionCallback =
        std::function<void(StreamLaunchResult, const std::string&)>;

    StreamLoadingActivity(std::shared_ptr<app::StreamController> ctrl,
                          StreamLaunchRequest request,
                          CompletionCallback completion);
    ~StreamLoadingActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    struct CancelContext;

    std::shared_ptr<app::StreamController> ctrl_;
    StreamLaunchRequest request_;
    std::shared_ptr<std::atomic<bool>> started_ =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> failure_pending_ =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<CancelContext> cancel_context_;
    brls::Box* loading_card_ = nullptr;
    brls::Box* error_card_ = nullptr;
    brls::Label* status_ = nullptr;
    brls::Label* detail_ = nullptr;
    brls::Label* error_detail_ = nullptr;
    brls::Label* footer_ = nullptr;
    std::string failure_detail_;

    void startConnection();
    void requestCancel();
    static void runCancelCleanup(
        const std::shared_ptr<CancelContext>& context,
        const std::shared_ptr<app::StreamController>& ctrl) noexcept;
    static bool scheduleCancelCleanup(
        const std::shared_ptr<CancelContext>& context,
        const std::shared_ptr<app::StreamController>& ctrl);
    void handleConnectionResult(bool ok, const std::string& error);
    void showFailure(const std::string& detail);
    void acknowledgeFailure();
    void finish(StreamLaunchResult result, const std::string& detail);
    void returnToMain();
};

} // namespace lunar::ui
#endif
