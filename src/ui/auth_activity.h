#pragma once
#ifdef __SWITCH__
#include <borealis.hpp>
#include "../app/stream_controller.h"
#include <thread>
#include <atomic>
#include <string>
#include <memory>

namespace lunar::ui {

class QrCodeView;

class AuthActivity : public brls::Activity {
public:
    explicit AuthActivity(std::shared_ptr<app::StreamController> ctrl = nullptr);
    ~AuthActivity();
    brls::View* createContentView() override;
    void onResume() override;
    static std::shared_ptr<app::StreamController> createConfiguredController();

private:
    std::shared_ptr<app::StreamController> ctrl_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> polling_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> poll_thread_done_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<uint32_t>> poll_generation_ = std::make_shared<std::atomic<uint32_t>>(0);
    std::shared_ptr<std::atomic<bool>> auth_requesting_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> auth_request_done_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<uint32_t>> auth_request_generation_ = std::make_shared<std::atomic<uint32_t>>(0);
    bool saved_session_resume_attempted_ = false;
    brls::Button* start_btn_ = nullptr;
    brls::Button* cancel_btn_ = nullptr;
    QrCodeView* qr_view_ = nullptr;
    brls::Label* url_label_ = nullptr;
    brls::Label* code_label_ = nullptr;
    brls::Label* status_label_ = nullptr;
    std::thread auth_request_thread_;
    std::thread poll_thread_;

    std::shared_ptr<app::StreamController> controller();
    void resetSignedOutUi();
    bool resumeSavedSessionIfPresent();
    void beginAuthRequest();
    void startPollingThread();
};

} // namespace lunar::ui
#endif
