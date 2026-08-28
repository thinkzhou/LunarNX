#ifdef __SWITCH__

#include "ps_remote_connector.h"
#include "ps_remote_retry_policy.h"
#include "../common.h"
#include "../diagnostics.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace lunar::ps {

namespace {

constexpr int kRemoteMaxAttempts = 3;

struct NetworkProfile {
    bool ryubing_compat = false;
    int fast_port_guessing_sockets = kPsFastNatSockets;
    int compatibility_port_guessing_sockets = kPsCompatibilityNatSockets;
    std::string relay_host;
    uint16_t relay_port = 0;
    std::string relay_secret;
};

NetworkProfile loadNetworkProfile() {
    NetworkProfile profile;
    FILE* input = std::fopen(lunar::get_config_path(), "rb");
    if (!input) return profile;

    cJSON* root = nullptr;
    if (std::fseek(input, 0, SEEK_END) == 0) {
        const long length = std::ftell(input);
        if (length > 0 && length <= 64 * 1024) {
            std::rewind(input);
            std::string data(static_cast<size_t>(length), '\0');
            if (std::fread(data.data(), 1, data.size(), input) == data.size()) {
                root = cJSON_Parse(data.c_str());
            }
        }
    }
    std::fclose(input);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return profile;
    }

    cJSON* profile_value =
        cJSON_GetObjectItemCaseSensitive(root, "ps_network_profile");
    if (profile_value && cJSON_IsString(profile_value) &&
        profile_value->valuestring &&
        std::string(profile_value->valuestring) == "ryubing") {
        profile.ryubing_compat = true;
        profile.fast_port_guessing_sockets = 32;

        cJSON* relay_host = cJSON_GetObjectItemCaseSensitive(root, "ps_relay_host");
        cJSON* relay_port = cJSON_GetObjectItemCaseSensitive(root, "ps_relay_port");
        cJSON* relay_secret = cJSON_GetObjectItemCaseSensitive(root, "ps_relay_secret");
        if (relay_host && cJSON_IsString(relay_host) && relay_host->valuestring &&
            relay_port && cJSON_IsNumber(relay_port) && relay_port->valueint > 0 &&
            relay_port->valueint <= 65535 && relay_secret && cJSON_IsString(relay_secret) &&
            relay_secret->valuestring && std::strlen(relay_secret->valuestring) >= 12 &&
            std::strlen(relay_secret->valuestring) < 129) {
            struct in_addr address{};
            if (inet_pton(AF_INET, relay_host->valuestring, &address) == 1) {
                profile.relay_host = relay_host->valuestring;
                profile.relay_port = static_cast<uint16_t>(relay_port->valueint);
                profile.relay_secret = relay_secret->valuestring;
            }
        }
    }

    cJSON* sockets =
        cJSON_GetObjectItemCaseSensitive(root, "ps_port_guessing_sockets");
    if (sockets && cJSON_IsNumber(sockets) && sockets->valueint > 0) {
        profile.fast_port_guessing_sockets =
            std::clamp(sockets->valueint, 1, 250);
        profile.compatibility_port_guessing_sockets = std::clamp(
            std::max(sockets->valueint, kPsCompatibilityNatSockets),
            1, 250);
    }
    cJSON_Delete(root);
    return profile;
}

bool isRetryableRemoteError(ChiakiErrorCode err) {
    switch (err) {
        case CHIAKI_ERR_UNKNOWN:
        case CHIAKI_ERR_NETWORK:
        case CHIAKI_ERR_CONNECTION_REFUSED:
        case CHIAKI_ERR_HOST_DOWN:
        case CHIAKI_ERR_HOST_UNREACH:
        case CHIAKI_ERR_DISCONNECTED:
        case CHIAKI_ERR_TIMEOUT:
        case CHIAKI_ERR_INVALID_RESPONSE:
        case CHIAKI_ERR_HTTP_NONOK:
            return true;
        default:
            return false;
    }
}

} // namespace

PsRemoteConnector::PsRemoteConnector(const std::string& psn_token, ChiakiLog* log)
    : psn_token_(psn_token), log_(log) {}

PsRemoteConnector::~PsRemoteConnector() {
    cancel();
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (session_) {
        chiaki_holepunch_session_fini(session_);
        session_ = nullptr;
    }
}

bool PsRemoteConnector::connect(const PsConnectionPlan& plan,
                                 StatusCallback on_status, PsRemoteResult& result) {
    // A cancel can arrive after the controller publishes this connector but
    // before this worker enters connect(). Never reset that request here.
    if (cancel_requested_.load()) {
        diagnosticLog("ps-remote", "connect skipped: already cancelled");
        return false;
    }
    // Preserve fields the caller set before invoking connect() (psn_account_id);
    // only reset the session fields this function owns.
    result.holepunch_session = nullptr;
    result.failed_phase.clear();
    result.error = CHIAKI_ERR_SUCCESS;
    result.attempts = 0;
    result.valid = false;

    if (!plan.isRemote()) {
        result.failed_phase = "connection plan";
        result.error = CHIAKI_ERR_INVALID_DATA;
        return false;
    }

    const NetworkProfile network = loadNetworkProfile();
    diagnosticLog("ps-remote", "target=%d console_type=%s",
                  plan.target, plan.isPs5() ? "PS5" : "PS4");
    diagnosticLog("ps-remote", "network profile=%s fast_sockets=%d fallback_sockets=%d",
                  network.ryubing_compat ? "ryubing" : "native_switch",
                  network.fast_port_guessing_sockets,
                  network.compatibility_port_guessing_sockets);

    if (network.ryubing_compat) {
        diagnosticLog("ps-remote", "Ryubing profile requires ps_relay_host, ps_relay_port and ps_relay_secret");
        if (on_status) on_status("Ryubing UDP relay is unavailable with Akira Chiaki");
        return false;
    }

    ChiakiHolepunchSession session = nullptr;
    PsRemoteRetryPolicy retry_policy(
        network.fast_port_guessing_sockets,
        network.compatibility_port_guessing_sockets);
    for (int attempt = 1; attempt <= kRemoteMaxAttempts; ++attempt) {
        result.attempts = attempt;
        if (on_status) {
            if (attempt == 1) {
                on_status("Initiating PSN session...");
            } else {
                on_status("Retrying PSN connection (" + std::to_string(attempt) +
                          "/" + std::to_string(kRemoteMaxAttempts) + ")...");
            }
        }

        session = chiaki_holepunch_session_init(psn_token_.c_str(), log_);
        if (!session) {
            diagnosticLog("ps-remote", "holepunch init failed attempt=%d", attempt);
            result.failed_phase = "initialization";
            result.error = CHIAKI_ERR_MEMORY;
            return false;
        }
        // Even when enabled, Chiaki only opens the guessing sockets after STUN
        // proves that the NAT rewrites ports unpredictably. Keep the first
        // attempt bounded and cheap; widen it only after a real CTRL candidate
        // failure.
        chiaki_holepunch_session_force_port_guessing(session, true);
        const PsRemoteAttemptProfile attempt_profile =
            retry_policy.attemptProfile();
        chiaki_holepunch_session_set_port_guessing_socks(
            session, attempt_profile.port_guessing_sockets);
        if (attempt_profile.port_guesses > 0) {
            chiaki_holepunch_session_set_port_guessing_ports(
                session, attempt_profile.port_guesses);
        }
        diagnosticLog("ps-remote", "attempt=%d nat_mode=%s sockets=%d guesses=%d",
                      attempt,
                      attempt_profile.mode == PsNatTraversalMode::Compatibility
                          ? "compatibility" : "fast",
                      attempt_profile.port_guessing_sockets,
                      attempt_profile.port_guesses);
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_ = session;
            if (cancel_requested_.load()) {
                chiaki_holepunch_main_thread_cancel(session_, true);
            }
        }

        const char* failed_phase = nullptr;
        ChiakiErrorCode err = cancel_requested_.load()
            ? CHIAKI_ERR_CANCELED : CHIAKI_ERR_SUCCESS;
        if (err == CHIAKI_ERR_SUCCESS && attempt_profile.discover_upnp) {
            if (on_status) on_status("Trying router-assisted NAT traversal...");
            const ChiakiErrorCode upnp_err =
                chiaki_holepunch_upnp_discover(session);
            if (upnp_err != CHIAKI_ERR_SUCCESS) {
                // UPnP is an optional candidate source. A router without UPnP
                // must still be allowed to proceed with STUN and port guessing.
                diagnosticLog("ps-remote", "UPnP discovery unavailable: %s",
                              chiaki_error_string(upnp_err));
            }
        }
        if (err == CHIAKI_ERR_SUCCESS) {
            if (on_status) on_status("Creating remote session...");
            failed_phase = "session create";
            err = chiaki_holepunch_session_create(session);
        }
        if (err == CHIAKI_ERR_SUCCESS && cancel_requested_.load()) {
            failed_phase = "PSN connection cancelled";
            err = CHIAKI_ERR_CANCELED;
        }
        if (err == CHIAKI_ERR_SUCCESS) {
            failed_phase = "control offer";
            err = holepunch_session_create_offer(session);
        }

        ChiakiHolepunchConsoleType dev_type = plan.isPs5()
            ? CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5
            : CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4;
        if (err == CHIAKI_ERR_SUCCESS) {
            if (on_status) on_status("Starting remote console...");
            failed_phase = "session start";
            err = chiaki_holepunch_session_start(
                session, plan.console_uid.data(), dev_type);
        }
        if (err == CHIAKI_ERR_SUCCESS && cancel_requested_.load()) {
            failed_phase = "PSN connection cancelled";
            err = CHIAKI_ERR_CANCELED;
        }
        if (err == CHIAKI_ERR_SUCCESS) {
            if (on_status) on_status(
                attempt_profile.mode == PsNatTraversalMode::Compatibility
                ? "Punching control channel with NAT compatibility..."
                : "Punching control channel...");
            failed_phase = "control punch";
            err = chiaki_holepunch_session_punch_hole(
                session, CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL);
        }
        if (err == CHIAKI_ERR_SUCCESS && cancel_requested_.load()) {
            failed_phase = "PSN connection cancelled";
            err = CHIAKI_ERR_CANCELED;
        }
        if (err == CHIAKI_ERR_SUCCESS) {
            int32_t allocation_increment = -1;
            bool random_allocation = false;
            if (attempt_profile.mode == PsNatTraversalMode::Fast &&
                chiaki_holepunch_session_get_stun_allocation(
                    session, &allocation_increment, &random_allocation)) {
                diagnosticLog("ps-remote",
                              "CTRL STUN allocation increment=%d random=%d",
                              allocation_increment, random_allocation ? 1 : 0);
                retry_policy.recordStunAllocation(random_allocation);
            }

            if (attempt_profile.mode == PsNatTraversalMode::Fast &&
                retry_policy.natCompatibilityEnabled()) {
                // CTRL has already succeeded. Upgrade this same session before
                // Chiaki creates the DATA offer so audio/video gets a router
                // candidate and the wider bounded guessing window.
                const PsRemoteAttemptProfile data_profile =
                    retry_policy.attemptProfile();
                chiaki_holepunch_session_set_port_guessing_socks(
                    session, data_profile.port_guessing_sockets);
                chiaki_holepunch_session_set_port_guessing_ports(
                    session, data_profile.port_guesses);
                if (on_status) {
                    on_status("Preparing media channel for restrictive NAT...");
                }
                const ChiakiErrorCode upnp_err =
                    chiaki_holepunch_upnp_discover(session);
                if (upnp_err != CHIAKI_ERR_SUCCESS) {
                    diagnosticLog("ps-remote",
                                  "DATA UPnP discovery unavailable: %s",
                                  chiaki_error_string(upnp_err));
                }
                diagnosticLog("ps-remote",
                              "DATA NAT compatibility sockets=%d guesses=%d",
                              data_profile.port_guessing_sockets,
                              data_profile.port_guesses);
            }
            break;
        }

        diagnosticLog("ps-remote", "%s failed: %s attempt=%d/%d",
                      failed_phase ? failed_phase : "remote connection",
                      chiaki_error_string(err), attempt, kRemoteMaxAttempts);
        result.failed_phase = failed_phase ? failed_phase : "remote connection";
        result.error = err;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (session_ == session) session_ = nullptr;
        }
        chiaki_holepunch_session_fini(session);
        session = nullptr;

        const bool retry_transient = retry_policy.recordFailure(
            result.failed_phase,
            isRetryableRemoteError(err) && !cancel_requested_.load(),
            attempt < kRemoteMaxAttempts);
        if (!retry_transient) return false;

        if (attempt_profile.mode == PsNatTraversalMode::Fast &&
            retry_policy.natCompatibilityEnabled()) {
            diagnosticLog("ps-remote",
                          "CTRL candidates failed; enabling bounded UPnP and wider port guessing");
        }

        const auto backoff = std::chrono::milliseconds(200 * (1 << (attempt - 1)));
        diagnosticLog("ps-remote", "retrying full remote flow in %lld ms",
                      static_cast<long long>(backoff.count()));
        std::unique_lock<std::mutex> retry_lock(retry_mutex_);
        if (retry_cv_.wait_for(retry_lock, backoff, [this]() {
                return cancel_requested_.load();
            })) {
            result.failed_phase = "PSN connection cancelled";
            result.error = CHIAKI_ERR_CANCELED;
            return false;
        }
    }

    if (!session) return false;

    if (on_status) on_status("Control channel established");
    result.holepunch_session = session;
    result.failed_phase.clear();
    result.error = CHIAKI_ERR_SUCCESS;
    result.valid = true;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_ = nullptr;
    }
    diagnosticLog("ps-remote", "Control holepunch complete attempt=%d/%d",
                  result.attempts, kRemoteMaxAttempts);
    return true;
}

void PsRemoteConnector::cancel() {
    cancel_requested_ = true;
    retry_cv_.notify_all();
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (session_) {
        chiaki_holepunch_main_thread_cancel(session_, true);
    }
}

} // namespace lunar::ps

#endif
