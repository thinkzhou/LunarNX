#pragma once

#include <string>

namespace lunar::ps {

bool isValidPsnAccountId(const std::string& account_id);
bool normalizeBase64PsnAccountId(const std::string& input, std::string& account_id);
bool decimalPsnAccountIdToBase64(const std::string& input, std::string& account_id);
bool hexPsnAccountIdToBase64(const std::string& input, std::string& account_id);
bool normalizePsnAccountId(const std::string& input, std::string& account_id);
#ifdef __SWITCH__
std::string loadManualPsnAccountId(const std::string& console_key = {});
bool saveManualPsnAccountId(const std::string& account_id,
                            const std::string& console_key = {});
#endif

} // namespace lunar::ps
