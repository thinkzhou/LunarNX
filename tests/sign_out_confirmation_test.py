#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    main_ui = (ROOT / "src/ui/main_activity.cpp").read_text()
    main_header = (ROOT / "src/ui/main_activity.h").read_text()
    ps_ui = (ROOT / "src/ui/ps_activity.cpp").read_text()
    ps_header = (ROOT / "src/ui/ps_activity.h").read_text()

    assert "confirmSignOut();" in main_ui
    assert "void confirmSignOut();" in main_header
    assert "new brls::Dialog(" in main_ui
    assert "sign_out_confirm_message" in main_ui
    assert "resetToAuthActivity();" in main_ui
    assert "confirmSignOut();" in ps_ui
    assert "void confirmSignOut();" in ps_header
    assert "new brls::Dialog(" in ps_ui
    assert "sign_out_confirm_message" in ps_ui
    assert 'ps_manager_->psnAuth().signOut();' in ps_ui

    for language in ("en-US", "zh-Hans", "zh-Hant"):
        text = (ROOT / f"romfs/i18n/{language}/lunarnx.json").read_text()
        assert '"sign_out_confirm_message"' in text

    print("Sign-out confirmation tests passed")


if __name__ == "__main__":
    main()
