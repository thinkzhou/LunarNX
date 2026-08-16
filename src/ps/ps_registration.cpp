#ifdef __SWITCH__

#include "ps_registration.h"
#include "../diagnostics.h"
#include <chiaki/base64.h>
#include <cstring>

namespace lunar::ps {
namespace {

std::string registrationFailureDetail(const char* log) {
    if (!log) return "Registration failed";
    const std::string messages(log);
    if (messages.find("failed to generate random ambassador") != std::string::npos) {
        return "Could not initialize registration encryption";
    }
    if (messages.find("failed to format payload") != std::string::npos ||
        messages.find("failed to format request") != std::string::npos) {
        return "Could not prepare the registration request";
    }
    if (messages.find("failed to getaddrinfo") != std::string::npos) {
        return "Invalid console address";
    }
    if (messages.find("failed to create socket for search") != std::string::npos) {
        return "Local pairing socket unavailable; restart LunarNX and try again";
    }
    if (messages.find("failed to connect for search") != std::string::npos ||
        messages.find("connect failed: tried all addresses") != std::string::npos) {
        return "Could not reach the console pairing port; check its IP address and network";
    }
    if (messages.find("timed out waiting for search response") != std::string::npos ||
        messages.find("Regist search failed") != std::string::npos) {
        return "Console did not answer the pairing search; check pairing mode and console type";
    }
    if (messages.find("failed to connect for request") != std::string::npos) {
        return "Could not connect to the console registration port";
    }
    if (messages.find("failed to send request header") != std::string::npos ||
        messages.find("failed to send payload") != std::string::npos) {
        return "Could not send the registration request";
    }
    if (messages.find("received HTTP code") != std::string::npos ||
        messages.find("Reported Application Reason") != std::string::npos) {
        return "Console rejected registration; check the active user's Account ID and PIN";
    }
    if (messages.find("failed to receive response HTTP header") != std::string::npos) {
        return "Timed out waiting for the console registration response";
    }
    if (messages.find("response does not contain") != std::string::npos ||
        messages.find("failed to pare response") != std::string::npos) {
        return "Console returned an invalid registration response";
    }
    return "Registration failed; check the active user's Account ID, PIN, and console type";
}

const char* registrationFailureStage(const char* log) {
    if (!log) return "unknown";
    const std::string messages(log);
    if (messages.find("failed to generate random ambassador") != std::string::npos)
        return "crypto";
    if (messages.find("failed to format payload") != std::string::npos ||
        messages.find("failed to format request") != std::string::npos)
        return "request-format";
    if (messages.find("failed to getaddrinfo") != std::string::npos)
        return "invalid-address";
    if (messages.find("failed to create socket for search") != std::string::npos)
        return "search-socket";
    if (messages.find("failed to connect for search") != std::string::npos ||
        messages.find("connect failed: tried all addresses") != std::string::npos)
        return "search-connect";
    if (messages.find("timed out waiting for search response") != std::string::npos ||
        messages.find("Regist search failed") != std::string::npos)
        return "search-timeout";
    if (messages.find("failed to connect for request") != std::string::npos)
        return "request-connect";
    if (messages.find("failed to send request header") != std::string::npos ||
        messages.find("failed to send payload") != std::string::npos)
        return "request-send";
    if (messages.find("received HTTP code") != std::string::npos ||
        messages.find("Reported Application Reason") != std::string::npos)
        return "console-rejected";
    if (messages.find("failed to receive response HTTP header") != std::string::npos)
        return "response-timeout";
    if (messages.find("response does not contain") != std::string::npos ||
        messages.find("failed to pare response") != std::string::npos)
        return "invalid-response";
    return "unknown";
}

} // namespace

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
        CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR, log_);
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
            persistentEventLog("ps-registration",
                "failed stage=%s target=%d",
                registrationFailureStage(
                    chiaki_log_sniffer_get_buffer(&self->log_sniffer_)),
                static_cast<int>(self->regist_.info.target));
            if (auto callback = std::move(self->callback_)) {
                callback(RegistrationResult::Failed,
                    registrationFailureDetail(
                        chiaki_log_sniffer_get_buffer(&self->log_sniffer_)));
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
