#ifdef __SWITCH__

#include "ps_pairing_account.h"
#include "ps_pairing_account_store.h"
#include "../common.h"

namespace lunar::ps {

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
