#!/usr/bin/env python3
from pathlib import Path


def main() -> None:
    activity = Path("src/ui/ps_registration_activity.cpp").read_text()
    ps_activity = Path("src/ui/ps_activity.cpp").read_text()
    ps_header = Path("src/ui/ps_activity.h").read_text()

    assert "setCustomNavigationRoute" in activity
    assert "digit_buttons" in activity
    assert "std::vector<brls::View*> row_buttons;" in activity
    assert "digit_buttons[digit - '0'] = btn;" in activity
    assert "row_buttons.push_back(btn);" in activity
    assert "std::vector<brls::View*> digit_buttons;" not in activity
    assert "FocusDirection::UP" in activity
    assert "FocusDirection::RIGHT" in activity

    assert "void PsActivity::onPause()" in ps_activity
    assert "console_list_refresh_suspended_" in ps_header
    assert "console_list_refresh_pending_" in ps_header
    assert "if (console_list_refresh_suspended_)" in ps_activity
    assert "brls::Application::giveFocus(lan_button_)" in ps_activity

    resume = ps_activity.split("void PsActivity::onResume()", 1)[1].split(
        "void PsActivity::startLanDiscovery()", 1)[0]
    deferred_refresh = resume.split("brls::sync([this, alive]()", 1)[1]
    foreground_guard = "if (!alive->load() || console_list_refresh_suspended_) return;"
    assert foreground_guard in deferred_refresh
    assert deferred_refresh.index(foreground_guard) < deferred_refresh.index(
        "brls::Application::giveFocus(lan_button_)")
    assert deferred_refresh.index("console_list_refresh_pending_ = false;") < \
        deferred_refresh.index("brls::Application::giveFocus(lan_button_)")
    before_deferred_refresh = resume.split("brls::sync([this, alive]()", 1)[0]
    assert "console_list_refresh_pending_ = false;" not in before_deferred_refresh

    print("PS pairing focus tests passed")


if __name__ == "__main__":
    main()
