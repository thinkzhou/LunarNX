#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>
#include <string>

namespace lunar::ui {

class AboutActivity : public brls::Activity {
public:
    brls::View* createContentView() override;

private:
    enum class Tab { Project = 0, Changelog = 1, Community = 2, Support = 3 };

    Tab selected_tab_ = Tab::Project;
    brls::Box* content_ = nullptr;
    brls::Button* tab_project_ = nullptr;
    brls::Button* tab_changelog_ = nullptr;
    brls::Button* tab_community_ = nullptr;
    brls::Button* tab_support_ = nullptr;

    brls::Button* makeTabButton(const std::string& label);
    void selectTab(Tab tab, bool focus_tab);
    void updateTabStyles();
    brls::View* makeProjectTab();
    brls::View* makeChangelogTab();
    brls::View* makeCommunityTab();
    brls::View* makeSupportTab();
};

} // namespace lunar::ui
#endif
