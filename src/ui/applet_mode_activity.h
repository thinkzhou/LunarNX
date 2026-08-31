#pragma once

#ifdef __SWITCH__

#include <borealis.hpp>

namespace lunar::ui {

class AppletModeActivity final : public brls::Activity {
public:
    brls::View* createContentView() override;
};

} // namespace lunar::ui

#endif
