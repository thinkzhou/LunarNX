#include "ps/ps_console_resolver.h"

#include <cassert>

using namespace lunar::ps;

int main() {
    PsConsole console;
    console.local = PsLocalEndpoint{"192.0.2.20", 9295, PsConsoleState::Ready, false};

    auto route = PsConsoleResolver::resolve(console, false);
    assert(route.type == ResolvedRouteType::None);

    console.remote = PsRemoteEndpoint{"001122", "Living Room", true};
    route = PsConsoleResolver::resolve(console, true);
    assert(route.type == ResolvedRouteType::Remote);

    console.local->verified = true;
    route = PsConsoleResolver::resolve(console, true);
    assert(route.type == ResolvedRouteType::Local);
    assert(route.host_addr == "192.0.2.20");

    console.local->state = PsConsoleState::Standby;
    route = PsConsoleResolver::resolve(console, false);
    assert(route.type == ResolvedRouteType::Local);
    return 0;
}
