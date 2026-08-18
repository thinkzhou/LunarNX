#include "ps_pairing_account_store.h"

#include <cJSON.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace lunar::ps {
namespace {

constexpr const char* kLegacyAccountIdKey = "ps_local_account_id";
constexpr const char* kAccountIdsKey = "ps_local_account_ids";

cJSON* readConfig(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
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

bool writeConfig(const std::string& path, cJSON* root) {
    char* content = cJSON_Print(root);
    if (!content) return false;
    const std::string temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    const size_t length = std::strlen(content);
    const bool written = file && std::fwrite(content, 1, length, file) == length;
    if (file) std::fclose(file);
    cJSON_free(content);
    if (!written) {
        std::remove(temporary.c_str());
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace

std::string canonicalPairingConsoleKey(const std::string& console_key) {
    size_t begin = 0;
    while (begin < console_key.size() &&
           std::isspace(static_cast<unsigned char>(console_key[begin]))) ++begin;
    size_t end = console_key.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(console_key[end - 1]))) --end;
    std::string key = console_key.substr(begin, end - begin);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    std::string mac;
    mac.reserve(12);
    for (char c : key) {
        if (c == ':' || c == '-') continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            mac.clear();
            break;
        }
        mac.push_back(c);
    }
    if (mac.size() == 12) return mac;
    if (key.size() > 128) return {};
    return key;
}

std::string loadPairingAccountId(const std::string& config_path,
                                 const std::string& console_key) {
    cJSON* root = readConfig(config_path);
    const std::string key = canonicalPairingConsoleKey(console_key);
    const cJSON* value = nullptr;
    if (key.empty()) {
        value = cJSON_GetObjectItemCaseSensitive(root, kLegacyAccountIdKey);
    } else {
        const cJSON* accounts =
            cJSON_GetObjectItemCaseSensitive(root, kAccountIdsKey);
        if (cJSON_IsObject(accounts)) {
            value = cJSON_GetObjectItemCaseSensitive(accounts, key.c_str());
        }
    }
    const std::string result = cJSON_IsString(value) && value->valuestring
        ? value->valuestring : "";
    cJSON_Delete(root);
    return result;
}

bool savePairingAccountId(const std::string& config_path,
                          const std::string& console_key,
                          const std::string& account_id) {
    if (account_id.empty()) return false;
    cJSON* root = readConfig(config_path);
    const std::string key = canonicalPairingConsoleKey(console_key);
    if (key.empty()) {
        cJSON_DeleteItemFromObject(root, kLegacyAccountIdKey);
        cJSON_AddStringToObject(root, kLegacyAccountIdKey, account_id.c_str());
    } else {
        cJSON* accounts = cJSON_GetObjectItemCaseSensitive(root, kAccountIdsKey);
        if (!cJSON_IsObject(accounts)) {
            cJSON_DeleteItemFromObject(root, kAccountIdsKey);
            accounts = cJSON_AddObjectToObject(root, kAccountIdsKey);
        }
        cJSON_DeleteItemFromObject(accounts, key.c_str());
        cJSON_AddStringToObject(accounts, key.c_str(), account_id.c_str());
    }
    const bool ok = writeConfig(config_path, root);
    cJSON_Delete(root);
    return ok;
}

} // namespace lunar::ps
