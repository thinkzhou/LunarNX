#pragma once

#if defined(__SWITCH__) || defined(LUNARNX_DESKTOP_TEST)

#include "ps_console.h"
#include <optional>
#include <string>

namespace lunar::ps {

enum class ResolvedRouteType {
    Local,
    Remote,
    None,
};

struct ResolvedRoute {
    ResolvedRouteType type = ResolvedRouteType::None;
    std::string host_addr;       // for local
    std::string console_duid;    // for remote
    std::string error;
};

class PsConsoleResolver {
public:
    // Decide route for a console based on available endpoints and PSN token.
    // Design doc §6.3: fresh LAN first, remote fallback.
    static ResolvedRoute resolve(const PsConsole& console, bool has_psn_token);

    // Get human-readable route description for UI loading phases
    static std::string routeDescription(const ResolvedRoute& route);
};

} // namespace lunar::ps

#endif
