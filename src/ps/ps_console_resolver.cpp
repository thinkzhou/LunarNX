#if defined(__SWITCH__) || defined(LUNARNX_DESKTOP_TEST)

#include "ps_console_resolver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace lunar::ps {

namespace {

bool isValidIpv4Literal(const std::string& value) {
    in_addr address{};
    return !value.empty() && inet_pton(AF_INET, value.c_str(), &address) == 1;
}

} // namespace

ResolvedRoute PsConsoleResolver::resolve(const PsConsole& console, bool has_psn_token) {
    ResolvedRoute route;

    // A saved RP credential and a numeric local address are sufficient to
    // attempt a normal LAN session. Discovery state is informational and is
    // not proof that a persisted address is unusable.
    if (console.local.has_value() && console.credentials.has_value() &&
        isValidIpv4Literal(console.local->ip)) {
        route.type = ResolvedRouteType::Local;
        route.origin = console.local->verified
            ? ResolvedRouteOrigin::LanDiscovered
            : ResolvedRouteOrigin::LanPersisted;
        route.host_addr = console.local->ip;
        return route;
    }

    // The caller removes the other endpoint for an explicit Local/Remote tab.
    // This fallback only applies to a generic resolver call where no usable
    // local route was selected.
    if (has_psn_token && console.remote.has_value() &&
        console.remote->remoteplay_enabled) {
        route.type = ResolvedRouteType::Remote;
        route.origin = ResolvedRouteOrigin::PsnRemote;
        route.console_duid = console.remote->duid;
        return route;
    }

    // 3. No route available
    route.type = ResolvedRouteType::None;
    if (console.local.has_value() && !console.credentials.has_value()) {
        route.error = "Console is not paired for local play";
    } else if (console.local.has_value() && console.credentials.has_value() &&
               !isValidIpv4Literal(console.local->ip)) {
        route.error = "Paired console has no valid local address";
    } else if (!console.local.has_value() && !console.remote.has_value()) {
        route.error = "No local or remote route available";
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
    if (route.type == ResolvedRouteType::Local) {
        return route.origin == ResolvedRouteOrigin::LanPersisted
            ? "Trying the last known LAN address..."
            : "Connecting via LAN...";
    }
    switch (route.type) {
        case ResolvedRouteType::Local:  return "Connecting via LAN...";
        case ResolvedRouteType::Remote: return "Connecting via PSN...";
        case ResolvedRouteType::None:   return "No route available";
    }
    return "";
}

} // namespace lunar::ps

#endif
