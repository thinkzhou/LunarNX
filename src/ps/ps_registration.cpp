#ifdef __SWITCH__

#include "ps_registration.h"
#include "psn_auth_utils.h"
#include <cstring>

namespace lunar::ps {

PsRegistration::PsRegistration(ChiakiLog* log) : log_(log) {}

PsRegistration::~PsRegistration() {
    stop();
}

bool PsRegistration::start(const std::string& host, uint32_t pin, int target,
                           const std::string& psn_account_id,
                           ChiakiRegisteredHost* result_out, ResultCallback cb) {
    if (running_.load()) return false;

    callback_ = std::move(cb);
    result_out_ = result_out;

    ChiakiRegistInfo info{};
    info.target = static_cast<ChiakiTarget>(target);
    info.host = host.c_str();
    // This API is used with a user-supplied host address. Broadcast search
    // would ignore the selected IP and wait for a broadcast response.
    info.broadcast = false;
    info.pin = pin;
    info.console_pin = 0;
    // chiaki-ng uses PSN Account-ID for PS4 >= 9.0 as well as PS5. Only the
    // legacy PS4 8.x path uses Online-ID, which this UI does not request.
    if (target >= CHIAKI_TARGET_PS4_9) {
        std::string account_id_bytes;
        if (!base64Decode(psn_account_id, account_id_bytes) ||
            account_id_bytes.size() != CHIAKI_PSN_ACCOUNT_ID_SIZE) {
            return false;
        }
        std::memcpy(info.psn_account_id, account_id_bytes.data(),
                    CHIAKI_PSN_ACCOUNT_ID_SIZE);
    }

    ChiakiErrorCode err = chiaki_regist_start(&regist_, log_, &info, onRegistEvent, this);
    if (err != CHIAKI_ERR_SUCCESS) {
        callback_ = {};
        result_out_ = nullptr;
        return false;
    }

    initialized_.store(true);
    running_.store(true);
    return true;
}

void PsRegistration::stop() {
    running_.store(false);
    if (!initialized_.exchange(false)) return;

    chiaki_regist_stop(&regist_);
    chiaki_regist_fini(&regist_);
}

void PsRegistration::onRegistEvent(ChiakiRegistEvent* event, void* user) {
    auto* self = static_cast<PsRegistration*>(user);
    if (!self || !event) return;

    switch (event->type) {
        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS:
            if (self->result_out_ && event->registered_host) {
                std::memcpy(self->result_out_, event->registered_host, sizeof(ChiakiRegisteredHost));
            }
            if (self->callback_) {
                self->callback_(RegistrationResult::Success, "");
            }
            break;

        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED:
            if (self->callback_) {
                self->callback_(RegistrationResult::Failed, "Registration failed");
            }
            break;

        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED:
            if (self->callback_) {
                self->callback_(RegistrationResult::Cancelled, "Registration cancelled");
            }
            break;

        default:
            break;
    }
}

} // namespace lunar::ps

#endif
