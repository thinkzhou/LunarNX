#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("src/api/xbox_api_client.cpp").read_text()

    require('cJSON_AddStringToObject(settings, "osName", os_name);' in source,
            "Session osName must follow XStreaming's resolution-dependent value")
    require('cJSON_AddNumberToObject(input, "maxVersion", 8);' in source,
            "Input protocol maxVersion must match XStreaming")
    require('"reliableinput"' not in source,
            "SDP configuration must not advertise XStreaming-absent reliableinput")
    require('"unreliableinput"' not in source,
            "SDP configuration must not advertise XStreaming-absent unreliableinput")

    print("XStreaming SDP payload tests passed")


if __name__ == "__main__":
    main()
