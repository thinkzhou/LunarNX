#include "xbox_auth_errors.h"

#include <cJSON.h>
#include <iomanip>
#include <sstream>

namespace lunar::auth {
namespace {

std::string jsonValueString(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (!item) return "";
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    if (cJSON_IsNumber(item)) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(0) << item->valuedouble;
        return out.str();
    }
    return "";
}

} // namespace

std::string describeXstsFailure(int status, const std::string& body) {
    std::string xerr;
    std::string redirect;

    cJSON* root = cJSON_Parse(body.c_str());
    if (root) {
        xerr = jsonValueString(root, "XErr");
        redirect = jsonValueString(root, "Redirect");
        cJSON_Delete(root);
    }

    if (status == 401 && xerr == "2148916233") {
        if (redirect.empty()) redirect = "https://start.ui.xboxlive.com/CreateAccount";
        return "This Microsoft account needs an Xbox profile first. Create one at " +
               redirect + ", then try again.";
    }

    return "Xbox streaming authorization failed. HTTP " + std::to_string(status) + ".";
}

} // namespace lunar::auth
