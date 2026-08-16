#ifdef __SWITCH__

#include "ps_pairing_account.h"
#include "ps_pairing_account_store.h"
#include "psn_auth_utils.h"
#include "../common.h"
#include "../api/http_client.h"

#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>

namespace lunar::ps {
namespace {

std::string trim(const std::string& input) {
    const auto first = std::find_if_not(input.begin(), input.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string();
}

std::string urlEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[c >> 4]);
            encoded.push_back(hex[c & 0x0f]);
        }
    }
    return encoded;
}

} // namespace

bool lookupPsnAccountId(const std::string& username, std::string& account_id,
                        std::string& error) {
    error.clear();
    const std::string value = trim(username);
    if (value.empty() || value.size() > 64) {
        error = "Invalid PSN username";
        return false;
    }
    lunar::api::HttpClient client;
    const std::string base_url = "https://psn.flipscreen.games/search.php?username=";
    const auto response = client.getSensitive(
        base_url + urlEncode(value), base_url + "(redacted)");
    if (response.network_error || response.status_code != 200) {
        error = response.network_error ? response.error_message
            : "Lookup returned HTTP " + std::to_string(response.status_code);
        return false;
    }
    if (response.body.size() > 64 * 1024) {
        error = "Lookup response was too large";
        return false;
    }
    cJSON* root = cJSON_Parse(response.body.c_str());
    const cJSON* encoded = root
        ? cJSON_GetObjectItemCaseSensitive(root, "encoded_id") : nullptr;
    if (cJSON_IsString(encoded) && encoded->valuestring &&
        isValidPsnAccountId(encoded->valuestring)) {
        account_id = encoded->valuestring;
        cJSON_Delete(root);
        return true;
    }
    const cJSON* message = root
        ? cJSON_GetObjectItemCaseSensitive(root, "error") : nullptr;
    error = cJSON_IsString(message) && message->valuestring
        ? message->valuestring : "PSN username was not found";
    if (root) cJSON_Delete(root);
    return false;
}

std::string loadManualPsnAccountId(const std::string& console_key) {
    const std::string result = loadPairingAccountId(
        lunar::get_config_path(), console_key);
    return isValidPsnAccountId(result) ? result : "";
}

bool saveManualPsnAccountId(const std::string& account_id,
                            const std::string& console_key) {
    std::string normalized;
    if (!normalizePsnAccountId(account_id, normalized)) return false;
    return savePairingAccountId(
        lunar::get_config_path(), console_key, normalized);
}

} // namespace lunar::ps

#endif
