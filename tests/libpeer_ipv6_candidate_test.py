#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PORTS_SOURCE = ROOT / "lib/libpeer/src/ports.c"
AGENT_SOURCE = ROOT / "lib/libpeer/src/agent.c"
CONFIG_HEADER = ROOT / "lib/libpeer/src/config.h"
ICE_SOURCE = ROOT / "lib/libpeer/src/ice.c"
SWITCH_MAKEFILE = ROOT / "Makefile.switch"
DESKTOP_MAKEFILE = ROOT / "Makefile.desktop"
TRACKED_PATCH = ROOT / "tools/libpeer_legacy/legacy-libpeer-switch.patch"


def validate_ports(text: str, label: str) -> None:
    assert "switch_get_ipv6_source_addr" in text, (
        f"{label}: Switch cannot discover a routable IPv6 host candidate"
    )
    assert "socket(AF_INET6, SOCK_DGRAM, 0)" in text, (
        f"{label}: IPv6 source discovery does not use the BSD routing table"
    )
    assert "getsockname(fd" in text, (
        f"{label}: IPv6 source discovery does not read the selected local address"
    )
    assert "IN6_IS_ADDR_LINKLOCAL" in text, (
        f"{label}: link-local IPv6 addresses can be advertised for public ICE"
    )


def main() -> None:
    validate_ports(PORTS_SOURCE.read_text(), "legacy libpeer ports.c")
    config = CONFIG_HEADER.read_text()
    assert "#ifndef CONFIG_IPV6" in config
    agent = AGENT_SOURCE.read_text()
    assert "IPv6 UDP socket unavailable" in agent
    assert 'LOGE("Failed to create IPv6 UDP socket.");\n    return ret;' not in agent
    ice = ICE_SOURCE.read_text()
    parse_start = ice.index("int ice_candidate_from_description")
    parse_end = ice.index("return 0;", parse_start)
    parser = ice[parse_start:parse_end]
    assert parser.index("addr_from_string(addrstring") < parser.index(
        "addr_set_port(&candidate->addr, port)"
    ), "ICE candidate port is set before the IPv6 address family is known"
    assert "-DCONFIG_IPV6=1" in SWITCH_MAKEFILE.read_text()
    assert "-DCONFIG_IPV6=1" in DESKTOP_MAKEFILE.read_text()

    patch = TRACKED_PATCH.read_text()
    added_lines = "\n".join(
        line[1:]
        for line in patch.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )
    validate_ports(added_lines, "tracked legacy patch")
    assert "#ifndef CONFIG_IPV6" in added_lines
    print("libpeer IPv6 candidate tests passed")


if __name__ == "__main__":
    main()
