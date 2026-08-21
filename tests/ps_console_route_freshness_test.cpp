#include "ps/ps_console_resolver.h"

#include <cassert>

using namespace lunar::ps;

int main() {
    PsConsole console;
    console.local = PsLocalEndpoint{"192.0.2.20", 9295, PsConsoleState::Ready, false};

    auto route = PsConsoleResolver::resolve(console, false);
    assert(route.type == ResolvedRouteType::None);
    assert(route.error == "Console is not paired for local play");

    RegisteredCredential credential{};
    credential.rp_regist_key[0] = 1;
    credential.rp_key[0] = 1;
    console.credentials = credential;
    route = PsConsoleResolver::resolve(console, false);
    assert(route.type == ResolvedRouteType::Local);
    assert(route.origin == ResolvedRouteOrigin::LanPersisted);
    assert(PsConsoleResolver::routeDescription(route) ==
           "Trying the last known LAN address...");

    console.remote = PsRemoteEndpoint{"001122", "Living Room", true};
    route = PsConsoleResolver::resolve(console, true);
    assert(route.type == ResolvedRouteType::Local);

    console.local->verified = true;
    route = PsConsoleResolver::resolve(console, true);
    assert(route.type == ResolvedRouteType::Local);
    assert(route.origin == ResolvedRouteOrigin::LanDiscovered);
    assert(route.host_addr == "192.0.2.20");

    console.local->state = PsConsoleState::Standby;
    route = PsConsoleResolver::resolve(console, false);
    assert(route.type == ResolvedRouteType::Local);

    console.local->ip = "not-an-ip";
    route = PsConsoleResolver::resolve(console, false);
    assert(route.type == ResolvedRouteType::None);
    assert(route.error == "Paired console has no valid local address");

    console.local.reset();
    route = PsConsoleResolver::resolve(console, true);
    assert(route.type == ResolvedRouteType::Remote);
    assert(route.origin == ResolvedRouteOrigin::PsnRemote);

    route = PsConsoleResolver::resolve(console, false);
    assert(route.type == ResolvedRouteType::None);
    assert(route.error == "Console not on LAN - sign in to PSN for remote play");
    return 0;
}
