#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


header = (ROOT / "src/ps/chiaki_crypto_init.h").read_text()
implementation = (ROOT / "src/ps/chiaki_crypto_init.cpp").read_text()
main = (ROOT / "src/main.cpp").read_text()
switch_makefile = (ROOT / "Makefile.switch").read_text()
chiaki_builder = (ROOT / "tools/chiaki_switch/build_in_container.sh").read_text()

require("void initializeChiakiCrypto();" in header,
        "Chiaki crypto initialization must have an explicit startup API")
require("#ifdef __SWITCH__" in implementation and
        "#include <src/crypto/libnx/gmac.h>" in implementation,
        "libnx GHASH declarations must remain Switch-only")
require("chiaki_libnx_set_ghash_mode(CHIAKI_LIBNX_GHASH_PMULL);" in implementation,
        "Switch startup must request PMULL GHASH")
require("chiaki_libnx_get_ghash_mode()" in implementation,
        "Switch startup must verify the active GHASH mode")
require("GHASH requested=PMULL active=%s" in implementation,
        "Switch startup must persist the requested and active GHASH modes")
require("chiaki-crypto-warning" in implementation and
        "PMULL unavailable; continuing with GHASH TABLE fallback" in implementation,
        "TABLE fallback must emit an explicit persistent warning")
require(main.index("lunar::ps::initializeChiakiCrypto();") <
        main.index("brls::Application::init()"),
        "PMULL must be selected before UI activities can construct PS sessions")
require("src/ps/chiaki_crypto_init.cpp" in switch_makefile,
        "Switch build must compile the Chiaki startup initializer")
require("CHIAKI_LIB_ENABLE_LIBNX_EXPERIMENTAL=ON" in chiaki_builder,
        "Chiaki Switch library must compile the PMULL implementation")

print("Chiaki PMULL runtime contract passed")
