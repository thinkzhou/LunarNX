#pragma once

#ifdef __SWITCH__

#include <string>

namespace lunar::ps {

bool isValidPsnAccountId(const std::string& account_id);
bool normalizePsnAccountId(const std::string& input, std::string& account_id);
bool lookupPsnAccountId(const std::string& username, std::string& account_id,
                        std::string& error);
std::string loadManualPsnAccountId();
bool saveManualPsnAccountId(const std::string& account_id);

} // namespace lunar::ps

#endif
