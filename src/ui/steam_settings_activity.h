#pragma once
#if defined(__SWITCH__) && LUNARNX_STEAMLINK
#include <borealis.hpp>
#include "../steamlink/steam_pointer.h"
#include <functional>
namespace lunar::ui {
struct SteamInputSettings {
    steamlink::TouchMode touch = steamlink::TouchMode::Trackpad;
    steamlink::GyroMode gyro = steamlink::GyroMode::Native;
};
SteamInputSettings loadSteamInputSettings();
bool saveSteamInputSettings(const SteamInputSettings& settings);
class SteamSettingsActivity : public brls::Activity {
public:
    explicit SteamSettingsActivity(SteamInputSettings settings,
        std::function<void(const SteamInputSettings&)> completion = {})
        : settings_(settings), completion_(std::move(completion)) {}
    brls::View* createContentView() override;
private:
    SteamInputSettings settings_;
    std::function<void(const SteamInputSettings&)> completion_;
    void closeSettings();
};
}
#endif
