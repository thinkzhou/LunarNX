#!/usr/bin/env python3
import re
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    makefile = Path("Makefile.switch").read_text()
    cmake = Path("CMakeLists.txt").read_text()
    docker_build_script = Path(
        "tools/chiaki_switch/build_in_docker.sh").read_text()
    container_build_script = Path(
        "tools/chiaki_switch/build_in_container.sh").read_text()
    stun_patch = Path("tools/chiaki_switch/lunarnx-chiaki-stun-order.patch").read_text()
    reliability_patch = Path("tools/chiaki_switch/lunarnx-chiaki-holepunch-reliability.patch").read_text()
    rtt_patch = Path("tools/chiaki_switch/lunarnx-chiaki-stream-rtt.patch").read_text()
    wrapper = Path("src/platform/chiaki_curl_compat.cpp").read_text()
    chiaki_root = Path("github_repos/chiaki-ng-fork")
    chiaki_holepunch = (chiaki_root / "lib/src/remote/holepunch.c").read_text()

    require("CURL_PROVIDER ?= moonlight" in makefile,
            "combined Switch builds must default to WebSocket-capable curl")
    require("$(CURL_LIB) -ljson-c -lminiupnpc" in makefile,
            "PSN Remote must link its real JSON and UPnP dependencies")
    require("src/platform/chiaki_curl_compat.cpp" in makefile,
            "Chiaki HTTP and WebSocket handles must honor the Switch TLS mode")
    require("--wrap=curl_easy_init" in makefile and
            "__wrap_curl_easy_init" in wrapper and
            "__real_curl_easy_init" in wrapper,
            "unpatched Akira curl handles must inherit LunarNX Switch TLS policy")
    require("CHIAKI_LIB_ENABLE_LIBNX_CRYPTO" in makefile,
            "Chiaki consumers must use the library's public crypto ABI define")
    require("build_in_container.sh" in docker_build_script and
            "lunarnx-chiaki-stun-order.patch" in container_build_script and
            "lunarnx-chiaki-holepunch-reliability.patch" in container_build_script and
            "lunarnx-chiaki-stream-rtt.patch" in container_build_script and
            "git -C \"$src\" apply" in container_build_script,
            "Switch Chiaki build must apply focused reliability patches")
    require("first_send_ms" in rtt_patch and
            "event.data_ack.rtt_ms" in rtt_patch and
            "CHIAKI_STREAM_CONNECTION_RTT_WINDOW" in rtt_patch,
            "Switch Chiaki build must expose live Takion ACK RTT samples")
    require("clear_notification(session, msg->notification)" in reliability_patch and
            "CURLE_OPERATION_TIMEDOUT" in reliability_patch and
            "!short_msg || res != CURLE_OPERATION_TIMEDOUT" in reliability_patch and
            "port_type == CHIAKI_HOLEPUNCH_PORT_TYPE_DATA ? 3 : 1" in reliability_patch and
            "holepunch_session_create_offer(session)" in reliability_patch and
            "err = send_offer(session)" in reliability_patch and
            "session->quit_reason = CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN" in reliability_patch,
            "focused hole-punch reliability patch must cover queue cleanup, ACK-only HTTP retry, checked offers, bounded DATA retry, and explicit failure state")
    stun_order = [
        "stun.sonetel.com", "stun.siptrunk.com", "stun.romancecompass.com",
        "stun.axialys.net", "stun.flashdance.cx", "stun.sip.us",
        "stun.galeriemagnet.at", "stun.moonlight-stream.org",
    ]
    require(all(stun_patch.find(host) < stun_patch.find(stun_order[i + 1])
                for i, host in enumerate(stun_order[:-1])),
            "focused STUN patch must preserve measured server ordering")
    require("chiaki_holepunch_stubs.c" not in cmake,
            "Switch CMake builds must not replace live PSN dependencies with stubs")
    require("CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4" in wrapper,
            "Chiaki Switch HTTP requests must avoid unusable IPv6 routes")
    require("curl_ws_send" in wrapper and "curl_ws_recv" in wrapper,
            "Switch curl compatibility wrapper must expose WebSocket calls")
    require("lib/stubs/chiaki_holepunch_stubs.c" not in makefile,
            "Switch builds must not replace live PSN Remote dependencies with stubs")
    require("chiaki_holepunch_session_start" in chiaki_holepunch and
            "CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL" in chiaki_holepunch,
            "Akira Chiaki must retain the native PSN hole-punch flow")
    require("chiaki_holepunch_session_set_relay" not in chiaki_holepunch,
            "LunarNX relay extension must not be present in the Akira source")

    print("PS Switch runtime dependency tests passed")


if __name__ == "__main__":
    main()
