#pragma once

#if defined(__SWITCH__) || defined(LUNARNX_DESKTOP_TEST)

#include "ps_console.h"

#include <array>
#include <optional>
#include <string>

namespace lunar::ps {

enum class PsConnectionPreference {
    Automatic,
    LocalOnly,
    RemoteOnly,
};

enum class PsConnectionPlanType {
    LocalPS4,
    LocalPS5,
    RemotePS4,
    RemotePS5,
    None,
};

struct PsConnectionPlan {
    PsConnectionPlanType type = PsConnectionPlanType::None;
    int target = 0;
    std::string host_addr;
    std::array<uint8_t, 32> console_uid{};
    bool has_console_uid = false;
    std::optional<RegisteredCredential> credentials;
    std::string error;

    bool isLocal() const;
    bool isRemote() const;
    bool isPs5() const;
};

class PsConnectionPlanner {
public:
    static PsConnectionPlan makePlan(
        const PsConsole& console,
        bool has_psn_session,
        PsConnectionPreference preference = PsConnectionPreference::Automatic);

    static std::string describe(const PsConnectionPlan& plan);
};

} // namespace lunar::ps

#endif
