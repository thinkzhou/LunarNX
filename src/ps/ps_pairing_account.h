#pragma once

#ifdef __SWITCH__

#include <string>

namespace lunar::ps {

bool isValidPsnAccountId(const std::string& account_id);
std::string loadManualPsnAccountId();
bool saveManualPsnAccountId(const std::string& account_id);

} // namespace lunar::ps

#endif
