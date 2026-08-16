#include "ps/ps_credentials.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using lunar::ps::PsCredentials;
using lunar::ps::RegisteredCredential;

namespace {

RegisteredCredential credential(const char* mac, uint8_t marker) {
    RegisteredCredential value;
    value.server_mac = mac;
    value.nickname = marker == 1 ? "old" : "new";
    value.last_known_addr = "192.0.2.10";
    value.target = 1000100;
    std::memset(value.rp_regist_key, marker, sizeof(value.rp_regist_key));
    std::memset(value.rp_key, marker, sizeof(value.rp_key));
    return value;
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string path = argv[1];

    PsCredentials credentials;
    const auto old_value = credential("001122334455", 1);
    const auto new_value = credential("001122334455", 2);

    assert(credentials.addAndSave(old_value, path));
    assert(credentials.findByMac(old_value.server_mac)->nickname == "old");

    // A path below a missing directory forces persistence to fail. The
    // in-memory credential must remain the previously durable value.
    assert(!credentials.addAndSave(new_value, path + "/missing/credentials.json"));
    assert(credentials.findByMac(old_value.server_mac)->nickname == "old");

    PsCredentials reloaded;
    assert(reloaded.load(path));
    assert(reloaded.findByMac(old_value.server_mac)->nickname == "old");

    std::remove(path.c_str());
    return 0;
}
