#include "ps/ps_connection_plan.h"

#include <cassert>
#include <cstring>

using namespace lunar::ps;

int main() {
    RegisteredCredential ps4_credential;
    ps4_credential.target = 1000;
    std::memcpy(ps4_credential.rp_regist_key, "12345678", 8);

    PsConsole ps4;
    ps4.target = 1000;
    ps4.credentials = ps4_credential;
    ps4.local = PsLocalEndpoint{
        "192.0.2.20", 9295, PsConsoleState::Ready, false};
    ps4.remote = PsRemoteEndpoint{
        PsRemoteEndpointKind::MainPS4, "", "Main PS4", true};

    auto plan = PsConnectionPlanner::makePlan(
        ps4, true, PsConnectionPreference::Automatic);
    assert(plan.type == PsConnectionPlanType::RemotePS4);
    assert(!plan.has_console_uid);

    ps4.local->verified = true;
    plan = PsConnectionPlanner::makePlan(
        ps4, true, PsConnectionPreference::Automatic);
    assert(plan.type == PsConnectionPlanType::LocalPS4);
    assert(plan.host_addr == "192.0.2.20");
    assert(plan.credentials.has_value());

    plan = PsConnectionPlanner::makePlan(
        ps4, true, PsConnectionPreference::RemoteOnly);
    assert(plan.type == PsConnectionPlanType::RemotePS4);
    assert(!plan.has_console_uid);
    assert(plan.target == kPs4RemoteTarget);

    plan = PsConnectionPlanner::makePlan(
        ps4, false, PsConnectionPreference::RemoteOnly);
    assert(plan.type == PsConnectionPlanType::None);
    assert(plan.error.find("PSN") != std::string::npos);

    ps4.remote->remoteplay_enabled = false;
    plan = PsConnectionPlanner::makePlan(
        ps4, true, PsConnectionPreference::RemoteOnly);
    assert(plan.type == PsConnectionPlanType::None);
    assert(plan.error.find("enabled") != std::string::npos);
    ps4.remote->remoteplay_enabled = true;

    PsConsole ps5;
    ps5.target = 1000100;
    RegisteredCredential ps5_credential;
    ps5_credential.target = 1000100;
    ps5.credentials = ps5_credential;
    ps5.local = PsLocalEndpoint{
        "192.0.2.30", 9295, PsConsoleState::Standby, true};
    ps5.remote = PsRemoteEndpoint{
        PsRemoteEndpointKind::DevicePS5,
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f",
        "Living Room", true};

    plan = PsConnectionPlanner::makePlan(
        ps5, true, PsConnectionPreference::LocalOnly);
    assert(plan.type == PsConnectionPlanType::LocalPS5);

    plan = PsConnectionPlanner::makePlan(
        ps5, true, PsConnectionPreference::RemoteOnly);
    assert(plan.type == PsConnectionPlanType::RemotePS5);
    assert(plan.has_console_uid);
    assert(plan.target == kPs5RemoteTarget);
    assert(plan.console_uid[0] == 0x00);
    assert(plan.console_uid[31] == 0x1f);

    ps5.remote->duid = "invalid";
    plan = PsConnectionPlanner::makePlan(
        ps5, true, PsConnectionPreference::RemoteOnly);
    assert(plan.type == PsConnectionPlanType::None);
    assert(plan.error.find("ID") != std::string::npos);

    ps4.credentials.reset();
    plan = PsConnectionPlanner::makePlan(
        ps4, false, PsConnectionPreference::LocalOnly);
    assert(plan.type == PsConnectionPlanType::None);
    assert(plan.error.find("paired") != std::string::npos);

    // A reachable but unpaired LAN endpoint must not mask a valid PSN route.
    plan = PsConnectionPlanner::makePlan(
        ps4, true, PsConnectionPreference::Automatic);
    assert(plan.type == PsConnectionPlanType::RemotePS4);
    return 0;
}
