#if defined(__SWITCH__) || defined(LUNARNX_DESKTOP_TEST)

#include "ps_console_resolver.h"

namespace lunar::ps {

ResolvedRoute PsConsoleResolver::resolve(const PsConsole& console, bool has_psn_token) {
    ResolvedRoute route;

    // 1. If fresh LAN result exists, prefer local
    if (console.local.has_value() && console.local->verified) {
        if (console.local->state == PsConsoleState::Ready) {
            route.type = ResolvedRouteType::Local;
            route.host_addr = console.local->ip;
            return route;
        }
        if (console.local->state == PsConsoleState::Standby) {
            // Can wake and then connect locally
            route.type = ResolvedRouteType::Local;
            route.host_addr = console.local->ip;
            return route;
        }
    }

    // 2. Local unreachable, try remote fallback
    if (has_psn_token && console.remote.has_value() &&
        console.remote->remoteplay_enabled) {
        route.type = ResolvedRouteType::Remote;
        route.console_duid = console.remote->duid;
        return route;
    }

    // 3. No route available
    route.type = ResolvedRouteType::None;
    if ((!console.local.has_value() || !console.local->verified) &&
        !console.remote.has_value()) {
        route.error = "Console not available - not found on LAN or PSN";
    } else if (console.remote.has_value() && !console.remote->remoteplay_enabled) {
        route.error = "Remote play not enabled on this console";
    } else if (console.remote.has_value() && !has_psn_token) {
        route.error = "Console not on LAN - sign in to PSN for remote play";
    } else {
        route.error = "No route available";
    }
    return route;
}

std::string PsConsoleResolver::routeDescription(const ResolvedRoute& route) {
    switch (route.type) {
        case ResolvedRouteType::Local:  return "Connecting via LAN...";
        case ResolvedRouteType::Remote: return "Connecting via PSN...";
        case ResolvedRouteType::None:   return "No route available";
    }
    return "";
}

} // namespace lunar::ps

#endif
