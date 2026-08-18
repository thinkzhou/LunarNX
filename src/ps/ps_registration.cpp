#ifdef __SWITCH__

#include "ps_registration.h"
#include "ps_registration_diagnostics.h"
#include "../diagnostics.h"
#include <chiaki/base64.h>
#include <cstring>

namespace lunar::ps {
PsRegistration::PsRegistration(ChiakiLog* log) : log_(log) {}

PsRegistration::~PsRegistration() {
    stop();
}

ChiakiErrorCode PsRegistration::start(const std::string& host, uint32_t pin, int target,
                                      const std::string& psn_account_id,
                                      ChiakiRegisteredHost* result_out, ResultCallback cb) {
    if (running_.load()) return CHIAKI_ERR_UNINITIALIZED;

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
    // Chiaki uses Account-ID for PS4 system software 7.0+ and PS5. Its
    // CHIAKI_TARGET_PS4_9/PS4_10 names are protocol generations, not the
    // console firmware number shown in Sony's UI.
    if (target >= CHIAKI_TARGET_PS4_9) {
        size_t account_id_size = sizeof(info.psn_account_id);
        const ChiakiErrorCode decode_error = chiaki_base64_decode(
            psn_account_id.c_str(), psn_account_id.size(),
            info.psn_account_id, &account_id_size);
        if (decode_error != CHIAKI_ERR_SUCCESS ||
            account_id_size != CHIAKI_PSN_ACCOUNT_ID_SIZE) {
            persistentEventLog("ps-registration",
                "failed stage=invalid-account-id target=%d", target);
            return CHIAKI_ERR_INVALID_DATA;
        }
    }

    chiaki_log_sniffer_init(&log_sniffer_,
        CHIAKI_LOG_ALL, log_);
    log_sniffer_initialized_.store(true);
    ChiakiErrorCode err = chiaki_regist_start(
        &regist_, chiaki_log_sniffer_get_log(&log_sniffer_),
        &info, onRegistEvent, this);
    if (err != CHIAKI_ERR_SUCCESS) {
        persistentEventLog("ps-registration",
            "failed stage=worker-start target=%d error=%d", target,
            static_cast<int>(err));
        chiaki_log_sniffer_fini(&log_sniffer_);
        log_sniffer_initialized_.store(false);
        callback_ = {};
        result_out_ = nullptr;
        return err;
    }

    initialized_.store(true);
    running_.store(true);
    return CHIAKI_ERR_SUCCESS;
}

void PsRegistration::stop() {
    running_.store(false);
    if (initialized_.exchange(false)) {
        chiaki_regist_stop(&regist_);
        chiaki_regist_fini(&regist_);
    }
    if (log_sniffer_initialized_.exchange(false)) {
        chiaki_log_sniffer_fini(&log_sniffer_);
    }
}

void PsRegistration::onRegistEvent(ChiakiRegistEvent* event, void* user) {
    auto* self = static_cast<PsRegistration*>(user);
    if (!self || !event) return;

    switch (event->type) {
        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS:
            self->running_.store(false);
            if (self->result_out_ && event->registered_host) {
                std::memcpy(self->result_out_, event->registered_host, sizeof(ChiakiRegisteredHost));
            }
            if (auto callback = std::move(self->callback_)) {
                callback(RegistrationResult::Success, "");
            }
            break;

        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED:
            self->running_.store(false);
            {
                const char* captured_log =
                    chiaki_log_sniffer_get_buffer(&self->log_sniffer_);
                const RegistrationDiagnostic diagnostic =
                    analyzeRegistrationLog(captured_log);
                const std::string event = formatRegistrationEvent(
                    "failed", static_cast<int>(self->regist_.info.target),
                    captured_log);
                persistentEventLog("ps-registration", "%s", event.c_str());
                if (auto callback = std::move(self->callback_)) {
                    callback(RegistrationResult::Failed, diagnostic.detail);
                }
            }
            break;

        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED:
            self->running_.store(false);
            if (auto callback = std::move(self->callback_)) {
                callback(RegistrationResult::Cancelled, "Registration cancelled");
            }
            break;

        default:
            break;
    }
}

} // namespace lunar::ps

#endif
