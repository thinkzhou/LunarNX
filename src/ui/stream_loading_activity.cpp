#ifdef __SWITCH__
#include "stream_loading_activity.h"

#include "stream_view.h"
#include "ui_style.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"

#include <utility>

namespace lunar::ui {
namespace {

constexpr size_t kConnectStreamStackSize = 8 * 1024 * 1024;

std::string stateText(app::StreamState state, const std::string& info) {
    if (!info.empty()) return info;
    switch (state) {
        case app::StreamState::Authenticating:
            return brls::getStr("lunarnx/loading/refreshing_credentials");
        case app::StreamState::Connecting:
            return brls::getStr("lunarnx/loading/negotiating");
        case app::StreamState::Streaming:
            return brls::getStr("lunarnx/loading/stream_ready");
        case app::StreamState::Disconnected:
            return brls::getStr("lunarnx/loading/startup_stopped");
        case app::StreamState::Error:
            return brls::getStr("lunarnx/common/connection_failed");
        case app::StreamState::Idle:
        default:
            return brls::getStr("lunarnx/loading/preparing_session");
    }
}

} // namespace

StreamLoadingActivity::StreamLoadingActivity(
    std::shared_ptr<app::StreamController> ctrl,
    StreamLaunchRequest request,
    CompletionCallback completion)
    : ctrl_(std::move(ctrl)),
      request_(std::move(request)),
      completion_(std::move(completion)) {}

StreamLoadingActivity::~StreamLoadingActivity() {
    alive_->store(false);
}

brls::View* StreamLoadingActivity::createContentView() {
    const bool cloud = request_.target == StreamLaunchTarget::CloudTitle;
    const auto& p = uiPalette();

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
    root->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
    root->setPadding(30, 48, 24, 48);
    root->setBackgroundColor(nvgRGB(16, 16, 16));
    root->setFocusable(true);
    root->setHideHighlight(true);

    // The destination page owns focus, so repeated A presses cannot reach the
    // Play button on MainActivity while startup is in progress.
    root->registerClickAction([this](brls::View*) -> bool {
        if (failure_pending_->load()) acknowledgeFailure();
        return true;
    });
    root->registerAction(brls::getStr("lunarnx/loading/cancel_action"),
        brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            if (failure_pending_->load()) acknowledgeFailure();
            else requestCancel();
            return true;
        });
    auto acknowledge_error = [this](brls::View*) -> bool {
        if (!failure_pending_->load()) return false;
        acknowledgeFailure();
        return true;
    };
    for (auto button : {
             brls::ControllerButton::BUTTON_X,
             brls::ControllerButton::BUTTON_Y,
             brls::ControllerButton::BUTTON_START,
             brls::ControllerButton::BUTTON_BACK,
             brls::ControllerButton::BUTTON_LB,
             brls::ControllerButton::BUTTON_RB,
             brls::ControllerButton::BUTTON_LT,
             brls::ControllerButton::BUTTON_RT,
             brls::ControllerButton::BUTTON_LSB,
             brls::ControllerButton::BUTTON_RSB,
             brls::ControllerButton::BUTTON_UP,
             brls::ControllerButton::BUTTON_RIGHT,
             brls::ControllerButton::BUTTON_DOWN,
             brls::ControllerButton::BUTTON_LEFT,
             brls::ControllerButton::BUTTON_NAV_UP,
             brls::ControllerButton::BUTTON_NAV_RIGHT,
             brls::ControllerButton::BUTTON_NAV_DOWN,
             brls::ControllerButton::BUTTON_NAV_LEFT,
         }) {
        root->registerAction("", button, acknowledge_error, true);
    }

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setHeight(58);
    header->setAlignItems(brls::AlignItems::CENTER);

    auto* brand = new brls::Label();
    brand->setText("LUNARNX");
    brand->setFontSize(26);
    brand->setTextColor(p.accent);
    brand->setVerticalAlign(brls::VerticalAlign::CENTER);
    header->addView(brand);

    auto* product = new brls::Label();
    product->setText(cloud
        ? brls::getStr("lunarnx/common/xbox_cloud_gaming")
        : brls::getStr("lunarnx/common/xbox_remote_play"));
    product->setFontSize(15);
    product->setTextColor(p.text_muted);
    product->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    product->setVerticalAlign(brls::VerticalAlign::CENTER);
    product->setGrow(1.0f);
    header->addView(product);
    root->addView(header);

    auto* stage = new brls::Box(brls::Axis::COLUMN);
    stage->setGrow(1.0f);
    stage->setJustifyContent(brls::JustifyContent::CENTER);
    stage->setAlignItems(brls::AlignItems::CENTER);

    loading_card_ = new brls::Box(brls::Axis::COLUMN);
    loading_card_->setWidth(780);
    loading_card_->setHeight(430);
    loading_card_->setPadding(34, 46, 30, 46);
    loading_card_->setBackgroundColor(p.card);
    loading_card_->setBorderThickness(1);
    loading_card_->setBorderColor(p.border);
    loading_card_->setCornerRadius(8);
    loading_card_->setAlignItems(brls::AlignItems::CENTER);

    auto* eyebrow = new brls::Label();
    eyebrow->setText(brls::getStr("lunarnx/loading/starting"));
    eyebrow->setFontSize(14);
    eyebrow->setTextColor(p.accent);
    eyebrow->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    loading_card_->addView(eyebrow);

    auto* title = new brls::Label();
    title->setWidth(688);
    title->setHeight(52);
    title->setText(cloud
        ? brls::getStr("lunarnx/loading/launching", request_.display_name)
        : brls::getStr("lunarnx/loading/connecting_to", request_.display_name));
    title->setFontSize(30);
    title->setTextColor(p.text);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setVerticalAlign(brls::VerticalAlign::CENTER);
    loading_card_->addView(title);

    auto* target = new brls::Label();
    target->setWidth(688);
    target->setHeight(30);
    target->setText(cloud
        ? brls::getStr("lunarnx/loading/creating_cloud")
        : brls::getStr("lunarnx/loading/waking_xbox"));
    target->setFontSize(16);
    target->setTextColor(p.text_muted);
    target->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    target->setVerticalAlign(brls::VerticalAlign::CENTER);
    loading_card_->addView(target);

    auto* spinner = new brls::ProgressSpinner(brls::ProgressSpinnerSize::LARGE);
    spinner->setWidth(88);
    spinner->setHeight(88);
    loading_card_->addView(spinner);

    status_ = new brls::Label();
    status_->setWidth(688);
    status_->setHeight(48);
    status_->setText(cloud
        ? brls::getStr("lunarnx/loading/preparing_cloud")
        : brls::getStr("lunarnx/loading/preparing_remote"));
    status_->setFontSize(19);
    status_->setTextColor(p.text);
    status_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    status_->setVerticalAlign(brls::VerticalAlign::CENTER);
    loading_card_->addView(status_);

    detail_ = new brls::Label();
    detail_->setWidth(688);
    detail_->setHeight(44);
    detail_->setText(brls::getStr("lunarnx/loading/pipeline"));
    detail_->setFontSize(14);
    detail_->setTextColor(p.accent);
    detail_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    detail_->setVerticalAlign(brls::VerticalAlign::CENTER);
    loading_card_->addView(detail_);

    auto* wait_hint = new brls::Label();
    wait_hint->setWidth(688);
    wait_hint->setText(brls::getStr("lunarnx/loading/first_connection"));
    wait_hint->setFontSize(13);
    wait_hint->setTextColor(p.text_muted);
    wait_hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    loading_card_->addView(wait_hint);

    error_card_ = new brls::Box(brls::Axis::COLUMN);
    error_card_->setWidth(780);
    error_card_->setHeight(390);
    error_card_->setPadding(30, 54, 30, 54);
    error_card_->setBackgroundColor(p.card);
    error_card_->setBorderThickness(1);
    error_card_->setBorderColor(p.border);
    error_card_->setCornerRadius(8);
    error_card_->setAlignItems(brls::AlignItems::CENTER);
    error_card_->setVisibility(brls::Visibility::GONE);

    auto* error_eyebrow = new brls::Label();
    error_eyebrow->setText(brls::getStr("lunarnx/loading/error_eyebrow"));
    error_eyebrow->setFontSize(14);
    error_eyebrow->setTextColor(p.error);
    error_eyebrow->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    error_card_->addView(error_eyebrow);

    auto* error_mark = new brls::Label();
    error_mark->setWidth(66);
    error_mark->setHeight(66);
    error_mark->setText("!");
    error_mark->setFontSize(32);
    error_mark->setTextColor(p.error);
    error_mark->setBackgroundColor(p.surface_alt);
    error_mark->setCornerRadius(4);
    error_mark->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    error_mark->setVerticalAlign(brls::VerticalAlign::CENTER);
    error_card_->addView(error_mark);

    auto* error_title = new brls::Label();
    error_title->setWidth(672);
    error_title->setHeight(54);
    error_title->setText(brls::getStr("lunarnx/loading/error_title"));
    error_title->setFontSize(28);
    error_title->setTextColor(p.text);
    error_title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    error_title->setVerticalAlign(brls::VerticalAlign::CENTER);
    error_card_->addView(error_title);

    error_detail_ = new brls::Label();
    error_detail_->setWidth(672);
    error_detail_->setHeight(104);
    error_detail_->setFontSize(15);
    error_detail_->setTextColor(p.text_muted);
    error_detail_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    error_detail_->setVerticalAlign(brls::VerticalAlign::CENTER);
    error_card_->addView(error_detail_);

    auto* error_hint = new brls::Label();
    error_hint->setWidth(672);
    error_hint->setText(brls::getStr("lunarnx/loading/error_hint"));
    error_hint->setFontSize(14);
    error_hint->setTextColor(p.accent);
    error_hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    error_card_->addView(error_hint);

    stage->addView(loading_card_);
    stage->addView(error_card_);
    root->addView(stage);

    footer_ = new brls::Label();
    footer_->setHeight(34);
    footer_->setText(brls::getStr("lunarnx/loading/footer_cancel"));
    footer_->setFontSize(14);
    footer_->setTextColor(p.text_muted);
    footer_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    footer_->setVerticalAlign(brls::VerticalAlign::CENTER);
    root->addView(footer_);

    return root;
}

void StreamLoadingActivity::onContentAvailable() {
    auto alive = alive_;
    auto* self = this;
    ctrl_->setStateCallback([alive, self](app::StreamState state,
                                          const std::string& info) {
        std::string text = stateText(state, info);
        brls::sync([alive, self, state, text = std::move(text)]() {
            if (!alive->load()) return;
            if (self->status_ && !self->cancelling_->load()) {
                self->status_->setText(text);
            }
            if (self->detail_ && state == app::StreamState::Streaming) {
                self->detail_->setText(brls::getStr("lunarnx/loading/opening_view"));
            }
        });
    });

    // Match page-first navigation used by streaming clients: queue connection
    // startup only after Borealis has created the destination page.
    brls::sync([alive, self]() {
        if (!alive->load()) return;
        self->startConnection();
    });
}

void StreamLoadingActivity::startConnection() {
    if (started_->exchange(true) || cancelling_->load() || finished_->load()) {
        return;
    }

    const bool cloud = request_.target == StreamLaunchTarget::CloudTitle;
    lunar::diagnosticLog("ui-stream-start", "worker schedule type=%s target=%s",
                         cloud ? "cloud" : "home", request_.target_id.c_str());

    auto alive = alive_;
    auto ctrl = ctrl_;
    auto request = request_;
    auto* self = this;
    bool worker_started = lunar::platform::startNetworkWorker(
        cloud ? "connect-xcloud" : "connect-stream",
        [alive, ctrl, request = std::move(request), self]() {
            const bool is_cloud = request.target == StreamLaunchTarget::CloudTitle;
            bool ok = is_cloud
                ? ctrl->startCloudStream(request.target_id,
                                         request.width,
                                         request.height,
                                         request.options,
                                         request.bitrate_kbps)
                : ctrl->startStream(request.target_id,
                                    request.width,
                                    request.height,
                                    request.options,
                                    request.bitrate_kbps);
            std::string error = ok ? std::string() : ctrl->getLastStreamError();
            if (!alive->load()) {
                if (ok) {
                    lunar::platform::startNetworkWorker("stop-orphan-stream", [ctrl]() {
                        ctrl->stopStream(false);
                    });
                }
                return;
            }
            brls::sync([alive, self, ok, error = std::move(error)]() {
                if (!alive->load()) return;
                self->handleConnectionResult(ok, error);
            });
        },
        kConnectStreamStackSize);

    if (!worker_started) {
        handleConnectionResult(false, brls::getStr("lunarnx/loading/worker_failed"));
    }
}

void StreamLoadingActivity::requestCancel() {
    if (failure_pending_->load()) {
        acknowledgeFailure();
        return;
    }
    if (finished_->load() || cancelling_->exchange(true)) return;

    lunar::diagnosticLog("ui-stream-start", "cancel requested target=%s",
                         request_.target_id.c_str());
    if (status_) status_->setText(brls::getStr("lunarnx/loading/cancelling"));
    if (detail_) detail_->setText(brls::getStr("lunarnx/loading/stopping"));

    auto alive = alive_;
    auto ctrl = ctrl_;
    auto* self = this;
    bool worker_started = lunar::platform::startNetworkWorker(
        "cancel-connect",
        [alive, ctrl, self]() {
            ctrl->stopStream(false);
            brls::sync([alive, self]() {
                if (!alive->load()) return;
                self->finish(StreamLaunchResult::Cancelled,
                             brls::getStr("lunarnx/main/startup_cancelled"));
                self->returnToMain();
            });
        });

    if (!worker_started) {
        cancelling_->store(false);
        if (status_) status_->setText(brls::getStr("lunarnx/loading/cancel_failed"));
    }
}

void StreamLoadingActivity::handleConnectionResult(
    bool ok, const std::string& error) {
    if (finished_->load()) return;
    if (cancelling_->load()) {
        if (ok) {
            auto ctrl = ctrl_;
            lunar::platform::startNetworkWorker("stop-cancelled-stream", [ctrl]() {
                ctrl->stopStream(false);
            });
        }
        return;
    }

    if (!ok) {
        const std::string detail = error.empty()
            ? (request_.target == StreamLaunchTarget::CloudTitle
                   ? brls::getStr("lunarnx/loading/cloud_failed")
                   : brls::getStr("lunarnx/loading/remote_failed"))
            : error;
        lunar::diagnosticLog("ui-stream-start", "startup failed target=%s error=%s",
                             request_.target_id.c_str(), detail.c_str());
        showFailure(detail);
        return;
    }

    lunar::diagnosticLog("ui-stream-start", "startup ready target=%s",
                         request_.target_id.c_str());
    finish(StreamLaunchResult::Started, "");

    auto ctrl = ctrl_;
    auto open_stream = [ctrl]() {
        brls::Application::pushActivity(
            new StreamView(ctrl), brls::TransitionAnimation::NONE);
    };
    if (!brls::Application::popActivity(brls::TransitionAnimation::NONE,
                                        open_stream)) {
        open_stream();
    }
}

void StreamLoadingActivity::showFailure(const std::string& detail) {
    failure_detail_ = detail;
    failure_pending_->store(true);
    if (loading_card_) loading_card_->setVisibility(brls::Visibility::GONE);
    if (error_detail_) error_detail_->setText(detail);
    if (error_card_) error_card_->setVisibility(brls::Visibility::VISIBLE);
    if (footer_) footer_->setText(brls::getStr("lunarnx/loading/footer_error"));
}

void StreamLoadingActivity::acknowledgeFailure() {
    if (!failure_pending_->exchange(false)) return;
    lunar::diagnosticLog("ui-stream-start", "startup failure acknowledged target=%s",
                         request_.target_id.c_str());
    finish(StreamLaunchResult::Failed, failure_detail_);
    returnToMain();
}

void StreamLoadingActivity::finish(StreamLaunchResult result,
                                   const std::string& detail) {
    if (finished_->exchange(true)) return;
    if (completion_) completion_(result, detail);
}

void StreamLoadingActivity::returnToMain() {
    brls::Application::popActivity(brls::TransitionAnimation::NONE);
}

} // namespace lunar::ui
#endif
