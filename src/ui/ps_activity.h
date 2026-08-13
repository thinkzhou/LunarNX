#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>
#include "../ps/ps_manager.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace lunar::ui {

enum class PsConsoleSource {
    Local,
    Remote,
    Pair,
};

class PsActivity : public brls::Activity {
public:
    PsActivity();
    ~PsActivity() override;

    brls::View* createContentView() override;
    void onResume() override;

private:
    std::shared_ptr<ps::PsManager> ps_manager_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> psn_fetching_ =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> identity_fetching_ =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<uint64_t>> wake_generation_ =
        std::make_shared<std::atomic<uint64_t>>(0);

    brls::Box* console_list_ = nullptr;
    brls::Box* local_actions_ = nullptr;
    brls::Box* remote_actions_ = nullptr;
    brls::Box* pair_actions_ = nullptr;
    brls::Label* account_state_ = nullptr;
    brls::Label* lan_state_ = nullptr;
    brls::Label* psn_state_ = nullptr;
    brls::Label* action_status_ = nullptr;
    brls::Button* local_tab_ = nullptr;
    brls::Button* remote_tab_ = nullptr;
    brls::Button* pair_tab_ = nullptr;
    brls::Button* account_button_ = nullptr;
    brls::Button* lan_button_ = nullptr;
    brls::Button* psn_button_ = nullptr;
    bool resumed_once_ = false;
    bool had_psn_session_ = false;
    std::chrono::steady_clock::time_point back_navigation_ready_at_{};
    PsConsoleSource source_ = PsConsoleSource::Local;
    std::vector<ps::PsConsole> hosts_;
    std::string pending_wake_mac_;

    void startLanDiscovery();
    void fetchPsnConsoles();
    void stopDiscovery();
    void updateAccountUi();
    void setConsoleSource(PsConsoleSource source);
    void updateSourceUi();
    void rebuildConsoleList(const std::vector<ps::PsConsole>& hosts);
    bool completePendingWake(const std::vector<ps::PsConsole>& hosts);
    void pairConsole(const ps::PsConsole& host);
    void wakeupConsole(const ps::PsConsole& host);
    void connectToConsole(const ps::PsConsole& host);
    void showRemotePlayHelp();
    void handleAccountAction();
    void confirmSignOut();
    void signInToPsn();
};

} // namespace lunar::ui
#endif
