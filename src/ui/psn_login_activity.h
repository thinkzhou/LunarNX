#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>
#include "../ps/psn_auth_manager.h"
#include <atomic>
#include <memory>
#include <string>

namespace lunar::ui {

class PsnLoginActivity : public brls::Activity {
public:
    explicit PsnLoginActivity(ps::PsnAuthManager& auth);
    ~PsnLoginActivity() override;

    brls::View* createContentView() override;

private:
    ps::PsnAuthManager& auth_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);

    brls::Label* status_ = nullptr;
    brls::Box* manual_body_ = nullptr;
    brls::Button* manual_toggle_ = nullptr;
    bool manual_expanded_ = false;

    void openBrowser();
    void importCallbackFile();
    void exchangeInBackground(std::string input);
};

} // namespace lunar::ui
#endif
