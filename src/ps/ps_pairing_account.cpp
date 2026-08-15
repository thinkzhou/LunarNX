#ifdef __SWITCH__

#include "ps_pairing_account.h"
#include "psn_auth_utils.h"
#include "../common.h"

#include <cJSON.h>
#include <cstdio>
#include <cstring>

namespace lunar::ps {
namespace {

constexpr const char* kConfigKey = "ps_local_account_id";

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

std::string loadManualPsnAccountId() {
    cJSON* root = readConfig();
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, kConfigKey);
    std::string result = cJSON_IsString(value) && value->valuestring
        ? value->valuestring : "";
    cJSON_Delete(root);
    return isValidPsnAccountId(result) ? result : "";
}

bool saveManualPsnAccountId(const std::string& account_id) {
    if (!isValidPsnAccountId(account_id)) return false;
    cJSON* root = readConfig();
    cJSON_DeleteItemFromObject(root, kConfigKey);
    cJSON_AddStringToObject(root, kConfigKey, account_id.c_str());
    const bool ok = writeConfig(root);
    cJSON_Delete(root);
    return ok;
}

} // namespace lunar::ps

#endif
