#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>

#include "../steamlink/steam_link_client.h"
#include "../steamlink/steam_link_stream_controller.h"

#include <atomic>
#include <memory>

namespace lunar::ui {

class SteamLinkPairingActivity : public brls::Activity {
public:
    SteamLinkPairingActivity(std::shared_ptr<steamlink::SteamLinkClient> client,
                             steamlink::SteamLinkHost host);
    ~SteamLinkPairingActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    std::shared_ptr<steamlink::SteamLinkClient> client_;
    steamlink::SteamLinkHost host_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    brls::Label* pin_display_ = nullptr;
    brls::Label* status_ = nullptr;
    brls::InputCell* security_pin_input_ = nullptr;
    brls::Button* stream_button_ = nullptr;
    std::string pairing_pin_;
    std::string security_pin_;
    bool authorizing_ = false;
    bool starting_stream_ = false;
    std::shared_ptr<steamlink::SteamLinkStreamController> pending_runtime_;

    void startAuthorization();
    void startStream();
};

class SteamLinkActivity : public brls::Activity {
public:
    SteamLinkActivity();
    ~SteamLinkActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    std::shared_ptr<steamlink::SteamLinkClient> client_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    brls::Label* status_ = nullptr;
    brls::Box* host_list_ = nullptr;
    brls::Button* refresh_button_ = nullptr;

    void startDiscovery();
    void rebuildHosts(const std::vector<steamlink::SteamLinkHost>& hosts);
};

} // namespace lunar::ui
#endif
