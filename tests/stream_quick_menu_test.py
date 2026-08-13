#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    view = Path("src/ui/stream_view.cpp").read_text()
    header = Path("src/ui/stream_view.h").read_text()
    perf = Path("src/ui/perf_overlay.cpp").read_text()
    zh = Path("romfs/i18n/zh-Hans/lunarnx.json").read_text()
    en = Path("romfs/i18n/en-US/lunarnx.json").read_text()

    require("PanGestureRecognizer" in view and
            "ORIGINAL_WINDOW_WIDTH - kQuickMenuEdgeWidth" in view and
            "distance <= -kQuickMenuSwipeDistance" in view,
            "stream menu must open from a deliberate right-edge left swipe")
    require("(brls::Application::ORIGINAL_WINDOW_WIDTH - 520) / 2" in view and
            "(brls::Application::ORIGINAL_WINDOW_HEIGHT - 620) / 2" in view,
            "stream menu must use the approved centered dialog position")
    require("menu_handle_" not in view and "menu_handle_" not in header,
            "stream view must not permanently cover video with a menu handle")
    require("quick_menu_->setVisibility" in view and
            "quick_menu_visible_ || exit_pending_.load()" in view,
            "open menu must be visible and suppress game input")
    require("giveFocus(content_root_)" in view,
            "closing the menu must restore focus to the stream view")
    require("BUTTON_RSB" not in view and "toggle_stats" not in view,
            "R3 must remain exclusively available to the streamed game")
    require("menu_hide_performance" in view and
            "overlay_->setVisibility" in view and
            "perf_overlay_->setVisible(false)" in view,
            "menu must hide both compact and detailed performance overlays")
    require("getStreamPlatform() == app::StreamPlatform::PlayStation" in view and
            "menu_ps_button" in view and "menu_xbox_button" in view,
            "platform home button must use PlayStation or Xbox labeling")
    require("menu_stream_settings" in view and
            "new PsSettingsActivity" in view and
            "new StreamSettingsActivity" in view,
            "stream menu must open settings for the active platform")
    require("menu_button_mapping" in view and
            "ButtonMappingProfile::PlayStation" in view and
            "ButtonMappingProfile::Xbox" in view,
            "stream menu must open the active platform button mapping")
    require("menu_resume" in view and "setQuickMenuVisible(false)" in view,
            "stream menu must expose an explicit Resume action")
    require("kQuickDisconnectConfirmWindow" in view and
            "menu_disconnect_confirm" in view and
            "std::atomic<bool> disconnect_armed_" in header,
            "touch disconnect must require a second confirmation tap")
    require("quick_menu_" in header and "performance_visible_ = false" in header,
            "StreamView must own quick-menu visibility state")
    require("void PerfOverlay::setVisible" in perf,
            "detailed performance visibility must remain internally consistent")
    for text in (zh, en):
        for key in ("menu_title", "menu_hint", "menu_open", "menu_xbox_button",
                    "menu_ps_button", "menu_stream_settings",
                    "menu_button_mapping", "menu_resume",
                    "menu_hide_performance",
                    "menu_show_performance", "menu_disconnect",
                    "menu_disconnect_confirm"):
            require(f'"{key}"' in text, f"missing localized stream key: {key}")

    print("stream quick menu tests passed")


if __name__ == "__main__":
    main()
