#pragma once

#include <string>

namespace lunar::ps {

std::string canonicalPairingConsoleKey(const std::string& console_key);
std::string loadPairingAccountId(const std::string& config_path,
                                 const std::string& console_key);
bool savePairingAccountId(const std::string& config_path,
                          const std::string& console_key,
                          const std::string& account_id);

} // namespace lunar::ps
