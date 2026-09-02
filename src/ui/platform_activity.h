#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>
#include <chrono>

namespace lunar::ui {

class PlatformActivity : public brls::Activity {
public:
    PlatformActivity();

    brls::View* createContentView() override;
    void onResume() override;

private:
    std::chrono::steady_clock::time_point exit_navigation_ready_at_{};
    void openXbox();
    void openPlayStation();
};

} // namespace lunar::ui
#endif
