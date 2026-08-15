#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>
#include "../ps/ps_manager.h"
#include "../ps/ps_phone_pairing_server.h"
#include <atomic>
#include <memory>
#include <string>

namespace lunar::ui {

class QrCodeView;

class PsRegistrationActivity : public brls::Activity {
public:
    PsRegistrationActivity(std::shared_ptr<ps::PsManager> manager,
                           const std::string& host_addr, int target,
                           std::string host_name);
    ~PsRegistrationActivity() override;

    brls::View* createContentView() override;

private:
    std::shared_ptr<ps::PsManager> manager_;
    std::string host_addr_;
    int target_;
    std::string host_name_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> registering_ =
        std::make_shared<std::atomic<bool>>(false);
    ps::PsPhonePairingServer phone_pairing_server_;

    brls::Label* status_ = nullptr;
    brls::Label* pin_display_ = nullptr;
    QrCodeView* phone_qr_view_ = nullptr;
    std::string pin_buffer_;

    void onDigit(char digit);
    void onBackspace();
    void onSubmitPin();
    void startPhonePairing();
};

} // namespace lunar::ui
#endif
