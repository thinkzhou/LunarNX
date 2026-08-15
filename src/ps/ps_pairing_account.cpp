#ifdef __SWITCH__

#include "ps_pairing_account.h"
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

constexpr const char* kConfigKey = "ps_local_account_id";

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

cJSON* readConfig() {
    FILE* file = std::fopen(lunar::get_config_path(), "rb");
    if (!file) return cJSON_CreateObject();
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::rewind(file);
    std::string content(length > 0 ? static_cast<size_t>(length) : 0, '\0');
    if (!content.empty()) std::fread(content.data(), 1, content.size(), file);
    std::fclose(file);
    cJSON* root = cJSON_Parse(content.c_str());
    return root ? root : cJSON_CreateObject();
}

bool writeConfig(cJSON* root) {
    char* content = cJSON_Print(root);
    if (!content) return false;
    FILE* file = std::fopen(lunar::get_config_path(), "wb");
    const size_t length = std::strlen(content);
    const bool ok = file && std::fwrite(content, 1, length, file) == length;
    if (file) std::fclose(file);
    cJSON_free(content);
    return ok;
}

} // namespace

bool isValidPsnAccountId(const std::string& account_id) {
    std::string decoded;
    return base64Decode(account_id, decoded) && decoded.size() == 8;
}

bool normalizePsnAccountId(const std::string& input, std::string& account_id) {
    const auto first = std::find_if_not(input.begin(), input.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) return false;
    const std::string value(first, last);
    if (isValidPsnAccountId(value)) {
        account_id = value;
        return true;
    }
    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        })) {
        return false;
    }
    try {
        size_t consumed = 0;
        const unsigned long long uid = std::stoull(value, &consumed, 10);
        if (consumed != value.size()) return false;
        uint8_t bytes[8]{};
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = static_cast<uint8_t>((uid >> (i * 8)) & 0xff);
        }
        account_id = base64Encode(bytes, sizeof(bytes));
        return isValidPsnAccountId(account_id);
    } catch (...) {
        return false;
    }
}

bool lookupPsnAccountId(const std::string& username, std::string& account_id,
                        std::string& error) {
    error.clear();
    const auto first = std::find_if_not(username.begin(), username.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(username.rbegin(), username.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last || static_cast<size_t>(last - first) > 64) {
        error = "Invalid PSN username";
        return false;
    }
    const std::string value(first, last);
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

std::string loadManualPsnAccountId() {
    cJSON* root = readConfig();
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, kConfigKey);
    std::string result = cJSON_IsString(value) && value->valuestring
        ? value->valuestring : "";
    cJSON_Delete(root);
    return isValidPsnAccountId(result) ? result : "";
}

bool saveManualPsnAccountId(const std::string& account_id) {
    std::string normalized;
    if (!normalizePsnAccountId(account_id, normalized)) return false;
    cJSON* root = readConfig();
    cJSON_DeleteItemFromObject(root, kConfigKey);
    cJSON_AddStringToObject(root, kConfigKey, normalized.c_str());
    const bool ok = writeConfig(root);
    cJSON_Delete(root);
    return ok;
}

} // namespace lunar::ps

#endif
