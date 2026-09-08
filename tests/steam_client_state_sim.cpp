// Exercise the production Switch client with only libnx randomGet substituted.
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <cassert>
#include <iostream>
#define private public
#include "steamlink/steam_link_client.h"
#undef private
#include "../src/steamlink/steam_link_client.cpp"

int main() {
    using namespace lunar::steamlink;
    std::filesystem::create_directories("sdmc:/switch/LunarNX");
    // A page being destroyed must not shut down another page's timer service.
    {
        auto first = std::make_unique<SteamLinkClient>();
        auto second = std::make_unique<SteamLinkClient>();
        assert(first->ensureClient());
        assert(second->ensureClient());
        first.reset();
        assert(second->discoverAddress(""));
        second.reset();
    }
    SteamLinkClient client;
    IHS_HostInfo a{}, b{};
    a.clientId = 1; b.clientId = 2;
    a.address.ip.family = b.address.ip.family = IHS_IPAddressFamilyIPv4;
    a.address.ip.v4.data[0] = b.address.ip.v4.data[0] = 127;
    a.address.ip.v4.data[3] = b.address.ip.v4.data[3] = 1;
    a.address.port = b.address.port = 27036;
    std::vector<SteamLinkHost> visible;
    client.host_callback_ = [&](const auto& hosts) { visible = hosts; };
    client.updateHost(a); client.updateHost(b);
    assert(visible.size() == 2);
    SteamLinkClient::onAuthorizationSuccess(nullptr, &a, 100, &client);
    SteamLinkClient::onAuthorizationSuccess(nullptr, &b, 200, &client);
    assert(client.isAuthorized(1) && client.isAuthorized(2));
    SteamLinkClient::onStreamingFailed(nullptr, &a, IHS_StreamingPINRequired, &client);
    assert(client.isAuthorized(1)); // security PIN failure does not revoke pairing
    SteamLinkClient::onStreamingFailed(nullptr, &a, IHS_StreamingUnauthorized, &client);
    assert(!client.isAuthorized(1) && client.authorizedSteamId(1) == 0);
    assert(client.isAuthorized(2));
    SteamLinkClient::onAuthorizationSuccess(nullptr, &a, 101, &client);
    assert(client.isAuthorized(1) && client.authorizedSteamId(1) == 101);
    // Repeated status updates preserve identity and refresh the endpoint.
    a.address.ip.v4.data[3] = 2;
    client.updateHost(a);
    assert(visible.size() == 2 && visible[0].address == "127.0.0.2");
    client.last_seen_ms_[1] = 0;
    client.emitHosts();
    assert(visible.size() == 1 && visible[0].client_id == 2);
    IHS_HostInfo result{};
    assert(!client.findHost(1, &result));
    // Offline is not a revocation: a reappearing host can reconnect.
    client.updateHost(a);
    assert(client.isAuthorized(1) && visible.size() == 2);
    // Expiry ticks and network replies publish through the same serialized boundary.
    std::atomic<unsigned> active{0};
    client.host_callback_ = [&](const auto& hosts) {
        assert(active.fetch_add(1) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        visible = hosts;
        assert(active.fetch_sub(1) == 1);
    };
    std::thread replies([&] { for (int i=0; i<30; ++i) client.updateHost(a); });
    std::thread expiry([&] { for (int i=0; i<30; ++i) client.emitHosts(); });
    replies.join(); expiry.join();
    assert(visible.size() == 2);
    assert(client.discoverAddress(""));
    assert(!client.discoverAddress("not-an-ip"));
    assert(!client.discoverAddress("::1"));
    std::cout << "PASS: host expiry/reappearance/endpoint change, multi-host authorization/revocation/re-pair, invalid manual address\n";
}
