#include "../src/ps/ps_pairing_account_store.h"

#include <cassert>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string path = argv[1];
    const std::string ps4 = "7821365490ab";
    const std::string ps5 = "efcdab907856";
    const std::string id4 = "7821365490a=";
    const std::string id5 = "AbCdEf12345=";

    assert(lunar::ps::savePairingAccountId(path, "78:21:36:54:90:AB", id4));
    assert(lunar::ps::savePairingAccountId(path, "EF-CD-AB-90-78-56", id5));
    assert(lunar::ps::loadPairingAccountId(path, ps4) == id4);
    assert(lunar::ps::loadPairingAccountId(path, ps5) == id5);
    assert(lunar::ps::loadPairingAccountId(path, "001122334455").empty());

    // The legacy global value remains available only to keyless/manual flows;
    // it must never leak into a known console's lookup.
    assert(lunar::ps::savePairingAccountId(path, "", id4));
    assert(lunar::ps::loadPairingAccountId(path, "") == id4);
    assert(lunar::ps::loadPairingAccountId(path, "001122334455").empty());

    std::remove(path.c_str());
    return 0;
}
