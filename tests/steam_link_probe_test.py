#!/usr/bin/env python3
"""Behavioral tests for the desktop Steam Link discovery probe."""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.steamlink_probe import DISCOVERY_MAGIC, build_discovery_packet, parse_datagram


def varint(value: int) -> bytes:
    result = bytearray()
    while value >= 0x80:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value)
    return bytes(result)


def field_varint(number: int, value: int) -> bytes:
    return varint(number << 3) + varint(value)


def field_string(number: int, value: str) -> bytes:
    encoded = value.encode()
    return varint((number << 3) | 2) + varint(len(encoded)) + encoded


def frame(header: bytes, payload: bytes) -> bytes:
    return DISCOVERY_MAGIC + struct.pack("<I", len(header)) + header + struct.pack(
        "<I", len(payload)
    ) + payload


def test_discovery_packet_has_expected_wire_header() -> None:
    packet = build_discovery_packet(0x1234, 7)
    assert packet.startswith(DISCOVERY_MAGIC)
    assert b"\x08\xb4\x24" in packet  # client_id = 0x1234


def test_parse_status_datagram_returns_host_fields() -> None:
    header = field_varint(1, 0x1234) + field_varint(2, 1) + field_varint(3, 0x88)
    payload = (
        field_string(4, "Mac Steam")
        + field_varint(3, 27036)
        + field_varint(7, 16)
        + field_varint(11, 1)
        + field_varint(14, 1)
    )
    host = parse_datagram(frame(header, payload), ("192.168.1.20", 27036))
    assert host == {
        "client_id": 0x1234,
        "instance_id": 0x88,
        "hostname": "Mac Steam",
        "address": "192.168.1.20",
        "source_port": 27036,
        "connect_port": 27036,
        "ostype": 16,
        "euniverse": 1,
        "games_running": True,
    }


def test_parse_datagram_rejects_bad_magic_and_truncated_lengths() -> None:
    assert parse_datagram(b"bad", ("127.0.0.1", 1)) is None
    assert parse_datagram(DISCOVERY_MAGIC + b"\xff\xff\xff\xff", ("127.0.0.1", 1)) is None


if __name__ == "__main__":
    test_discovery_packet_has_expected_wire_header()
    test_parse_status_datagram_returns_host_fields()
    test_parse_datagram_rejects_bad_magic_and_truncated_lengths()
    print("steam link probe checks passed")
