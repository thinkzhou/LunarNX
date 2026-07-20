#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("src/api/xbox_api_client.cpp").read_text()
    makefile = Path("Makefile.switch").read_text()

    require("XBOX_RESPONSE_TRACE ?= 0" in makefile,
            "Switch builds must disable full Xbox response traces by default")
    require("-DLUNARNX_XBOX_RESPONSE_TRACE=$(XBOX_RESPONSE_TRACE)" in makefile,
            "Switch builds must expose an explicit Xbox response trace flag")
    require("#if LUNARNX_XBOX_RESPONSE_TRACE" in source,
            "Full Xbox response persistence must be compile-time guarded")

    require("xbox_responses.jsonl" in source,
            "Xbox response trace must use a dedicated local JSONL file")
    require('cJSON_AddStringToObject(entry, "body", response.body.c_str());' in source,
            "Xbox response trace must preserve the complete response body")

    stages = (
        "get-consoles",
        "create-session",
        "session-state",
        "session-configuration",
        "post-sdp",
        "get-sdp",
        "post-ice",
        "get-ice",
        "keepalive",
        "delete-session",
    )
    for stage in stages:
        require(f'traceXboxResponse("{stage}"' in source,
                f"Xbox response trace is missing stage: {stage}")

    require('"Authorization"' not in source[source.find("void traceXboxResponse"):source.find("void traceXboxResponse") + 2500],
            "Xbox response trace must not persist authorization headers")

    print("Xbox response trace tests passed")


if __name__ == "__main__":
    main()
