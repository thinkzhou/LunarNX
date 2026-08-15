#!/usr/bin/env python3
from pathlib import Path


def main() -> None:
    activity = Path("src/ui/ps_registration_activity.cpp").read_text()
    ps_activity = Path("src/ui/ps_activity.cpp").read_text()
    ps_header = Path("src/ui/ps_activity.h").read_text()

    assert "setCustomNavigationRoute" in activity
    assert "digit_buttons" in activity
    assert "FocusDirection::UP" in activity
    assert "FocusDirection::RIGHT" in activity

    assert "void PsActivity::onPause()" in ps_activity
    assert "console_list_refresh_suspended_" in ps_header
    assert "console_list_refresh_pending_" in ps_header
    assert "if (console_list_refresh_suspended_)" in ps_activity
    assert "brls::Application::giveFocus(lan_button_)" in ps_activity

    print("PS pairing focus tests passed")


if __name__ == "__main__":
    main()
