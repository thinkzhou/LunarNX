#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    source = (ROOT / "src/ui/auth_activity.cpp").read_text()
    header = (ROOT / "src/ui/auth_activity.h").read_text()

    create_start = source.index("brls::View* AuthActivity::createContentView()")
    create_end = source.index(
        "std::shared_ptr<app::StreamController> AuthActivity::controller()",
        create_start,
    )
    create_body = source[create_start:create_end]

    require(
        "resumeSavedSessionIfPresent();" not in create_body,
        "auth view construction must not restore a saved session",
    )
    require(
        "controller()" not in create_body,
        "auth view construction must not create the application controller",
    )

    available_start = source.index("void AuthActivity::onContentAvailable()")
    available_end = source.index(
        "std::shared_ptr<app::StreamController> AuthActivity::createConfiguredController()",
        available_start,
    )
    available_body = source[available_start:available_end]
    require(
        "if (resumeSavedSessionIfPresent())" in available_body,
        "the ready auth screen must try saved-session restoration before device-code auth",
    )
    require(
        available_body.index("resumeSavedSessionIfPresent")
        < available_body.index("beginAuthRequest"),
        "saved-session restoration must precede automatic device-code auth",
    )
    require(
        "void onContentAvailable() override;" in header,
        "AuthActivity must start sign-in only after its content is available",
    )
    require(
        "bool AuthActivity::resumeSavedSessionIfPresent()" in source,
        "saved-session restoration must report whether it navigated",
    )
    require(
        "bool resumeSavedSessionIfPresent();" in header,
        "AuthActivity must declare the saved-session navigation result",
    )

    print("Auth startup laziness tests passed")


if __name__ == "__main__":
    main()
