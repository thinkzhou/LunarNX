#if defined(__SWITCH__) || defined(LUNARNX_DESKTOP_TEST)

#include "ps_connection_plan.h"

namespace lunar::ps {
namespace {

bool localStateUsable(PsConsoleState state) {
    return state == PsConsoleState::Ready ||
           state == PsConsoleState::Standby;
}

PsConnectionPlan makeLocalPlan(const PsConsole& console) {
    PsConnectionPlan plan;
    if (!console.local.has_value() || !console.local->verified ||
        !localStateUsable(console.local->state) || console.local->ip.empty()) {
        plan.error = "No verified LAN route is available";
        return plan;
    }
    if (!console.credentials.has_value()) {
        plan.error = "Console is not paired for local play";
        return plan;
    }

    const int target = console.target != 0
        ? console.target : console.credentials->target;
    if (target <= 0) {
        plan.error = "Console protocol generation is unknown";
        return plan;
    }

    plan.type = isPs5Target(target)
        ? PsConnectionPlanType::LocalPS5
        : PsConnectionPlanType::LocalPS4;
    plan.target = target;
    plan.host_addr = console.local->ip;
    plan.credentials = console.credentials;
    return plan;
}

PsConnectionPlan makeRemotePlan(const PsConsole& console, bool has_psn_session) {
    PsConnectionPlan plan;
    if (!has_psn_session) {
        plan.error = "Sign in to PSN for remote play";
        return plan;
    }
    if (!console.remote.has_value()) {
        plan.error = "No PSN remote route is available";
        return plan;
    }
    if (!console.remote->remoteplay_enabled) {
        plan.error = "Remote play is not enabled on this console";
        return plan;
    }

    if (console.remote->kind == PsRemoteEndpointKind::MainPS4) {
        plan.type = PsConnectionPlanType::RemotePS4;
        plan.target = kPs4RemoteTarget;
        return plan;
    }

    if (!decodeDuid(console.remote->duid, plan.console_uid.data())) {
        plan.error = "PSN console ID is invalid";
        return plan;
    }
    plan.type = PsConnectionPlanType::RemotePS5;
    plan.target = kPs5RemoteTarget;
    plan.has_console_uid = true;
    return plan;
}

} // namespace

bool PsConnectionPlan::isLocal() const {
    return type == PsConnectionPlanType::LocalPS4 ||
           type == PsConnectionPlanType::LocalPS5;
}

bool PsConnectionPlan::isRemote() const {
    return type == PsConnectionPlanType::RemotePS4 ||
           type == PsConnectionPlanType::RemotePS5;
}

bool PsConnectionPlan::isPs5() const {
    return type == PsConnectionPlanType::LocalPS5 ||
           type == PsConnectionPlanType::RemotePS5;
}

PsConnectionPlan PsConnectionPlanner::makePlan(
    const PsConsole& console,
    bool has_psn_session,
    PsConnectionPreference preference) {
    if (preference == PsConnectionPreference::LocalOnly) {
        return makeLocalPlan(console);
    }
    if (preference == PsConnectionPreference::RemoteOnly) {
        return makeRemotePlan(console, has_psn_session);
    }

    PsConnectionPlan local = makeLocalPlan(console);
    if (local.isLocal()) return local;

    PsConnectionPlan remote = makeRemotePlan(console, has_psn_session);
    if (remote.isRemote()) return remote;

    // Prefer the failure from an endpoint the user can actually see. This
    // keeps errors actionable when a stale LAN entry and a PSN entry coexist.
    if (console.remote.has_value()) return remote;
    return local;
}

std::string PsConnectionPlanner::describe(const PsConnectionPlan& plan) {
    if (plan.isLocal()) return "Connecting via LAN...";
    if (plan.isRemote()) return "Connecting via PSN...";
    return "No route available";
}

} // namespace lunar::ps

#endif
