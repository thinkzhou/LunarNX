#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>

#include "../app/dev_bridge_client.h"

namespace lunar::ui {

class DevToolsActivity : public brls::Activity {
public:
    DevToolsActivity() = default;
    ~DevToolsActivity() override;
    brls::View* createContentView() override;

private:
    void refreshVersions();
    void renderVersions(const std::vector<app::DevBuild>& builds);
    void confirmInstall(app::DevBuild build);
    void installBuild(app::DevBuild build);
    void uploadLog();
    void setBusy(bool busy);

    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> busy_{false};
    brls::Label* status_ = nullptr;
    brls::Box* versions_ = nullptr;
    brls::Button* refresh_ = nullptr;
    brls::Button* upload_ = nullptr;
};

} // namespace lunar::ui
#endif
