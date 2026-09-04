#include "ps/ps_route_preferences.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <string>

using lunar::ps::PsRoutePreference;
using lunar::ps::PsRoutePreferenceStore;
using lunar::ps::psRoutePreferenceKey;

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string path = argv[1];
    std::remove(path.c_str());

    const std::array<uint8_t, 4> uid = {0x00, 0x7f, 0x80, 0xff};
    assert(psRoutePreferenceKey(uid.data(), uid.size()) == "007f80ff");
    assert(psRoutePreferenceKey(nullptr, 0).empty());

    PsRoutePreferenceStore store(path);
    assert(!store.load("console-a").hasPreferredStun());
    assert(!store.load("console-a").hasRemoteRoute());

    PsRoutePreference first;
    first.preferred_stun_host = "stun.sonetel.com";
    first.preferred_stun_port = 3478;
    first.remote_address = "203.0.113.10";
    first.remote_port = 54321;
    assert(store.save("console-a", first));

    const PsRoutePreference loaded_first = store.load("console-a");
    assert(loaded_first.preferred_stun_host == first.preferred_stun_host);
    assert(loaded_first.preferred_stun_port == first.preferred_stun_port);
    assert(loaded_first.remote_address == first.remote_address);
    assert(loaded_first.remote_port == first.remote_port);

    PsRoutePreference second;
    second.preferred_stun_host = "stun.siptrunk.com";
    second.preferred_stun_port = 3478;
    second.remote_address = "2001:db8::5";
    second.remote_port = 60000;
    assert(store.save("console-b", second));

    const PsRoutePreference reloaded_first = store.load("console-a");
    assert(reloaded_first.preferred_stun_host == second.preferred_stun_host);
    assert(reloaded_first.preferred_stun_port == second.preferred_stun_port);
    assert(reloaded_first.remote_address == first.remote_address);
    assert(reloaded_first.remote_port == first.remote_port);

    FILE* corrupt = std::fopen(path.c_str(), "wb");
    assert(corrupt);
    const char invalid[] = "{not-json";
    assert(std::fwrite(invalid, 1, sizeof(invalid) - 1, corrupt) ==
           sizeof(invalid) - 1);
    assert(std::fclose(corrupt) == 0);
    assert(!store.load("console-a").hasPreferredStun());
    assert(!store.load("console-a").hasRemoteRoute());

    std::remove(path.c_str());
    return 0;
}
