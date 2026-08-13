#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>
#include "../ps/psn_auth_manager.h"
#include "../ps/psn_callback_server.h"
#include <atomic>
#include <memory>
#include <string>

namespace lunar::ui {

class QrCodeView;

class PsnLoginActivity : public brls::Activity {
public:
    explicit PsnLoginActivity(ps::PsnAuthManager& auth);
    ~PsnLoginActivity() override;

    brls::View* createContentView() override;

private:
    ps::PsnAuthManager& auth_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    ps::PsnCallbackServer callback_server_;

    brls::Label* status_ = nullptr;
    QrCodeView* qr_view_ = nullptr;

    void startPhoneLogin();
    void exchangeInBackground(std::string input);
};

} // namespace lunar::ui
#endif
