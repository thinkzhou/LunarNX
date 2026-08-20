#include "chiaki_crypto_init.h"

#include "../diagnostics.h"

#ifdef __SWITCH__
#include <src/crypto/libnx/gmac.h>
#endif

namespace lunar::ps {

void initializeChiakiCrypto() {
#ifdef __SWITCH__
    chiaki_libnx_set_ghash_mode(CHIAKI_LIBNX_GHASH_PMULL);
    const auto active = chiaki_libnx_get_ghash_mode();
    const char* active_name =
        active == CHIAKI_LIBNX_GHASH_PMULL ? "PMULL" : "TABLE";
    persistentEventLog("chiaki-crypto",
                       "GHASH requested=PMULL active=%s",
                       active_name);
#endif
}

} // namespace lunar::ps
