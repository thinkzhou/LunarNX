#!/usr/bin/env python3
"""Small dependency-free Steam Remote Play discovery probe.

This deliberately stops at discovery. Authorization uses an encrypted ticket
and should go through ihslib or the LunarNX client.
"""

from __future__ import annotations

import argparse
import json
import random
import socket
import struct
import time
from typing import Any


DISCOVERY_MAGIC = bytes.fromhex("ffffffff214c5fa0")
DISCOVERY_MESSAGE = 0
STATUS_MESSAGE = 1


def _varint(value: int) -> bytes:
    result = bytearray()
    while value >= 0x80:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value)
    return bytes(result)


def _read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    for _ in range(10):
        if offset >= len(data):
            raise ValueError("truncated protobuf varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
        shift += 7
    raise ValueError("protobuf varint is too long")


def _field_varint(number: int, value: int) -> bytes:
    return _varint(number << 3) + _varint(value)


def build_discovery_packet(client_id: int, sequence: int) -> bytes:
    """Build one Steam Remote Play discovery datagram."""

    header = _field_varint(1, client_id) + _field_varint(2, DISCOVERY_MESSAGE)
    payload = _field_varint(1, sequence)
    return (
        DISCOVERY_MAGIC
        + struct.pack("<I", len(header))
        + header
        + struct.pack("<I", len(payload))
        + payload
    )


def _parse_fields(data: bytes) -> dict[int, list[Any]]:
    fields: dict[int, list[Any]] = {}
    offset = 0
    while offset < len(data):
        tag, offset = _read_varint(data, offset)
        number, wire_type = tag >> 3, tag & 0x07
        if number == 0:
            raise ValueError("invalid protobuf field number")
        if wire_type == 0:
            value, offset = _read_varint(data, offset)
        elif wire_type == 1:
            end = offset + 8
            if end > len(data):
                raise ValueError("truncated fixed64 field")
            value, offset = data[offset:end], end
        elif wire_type == 2:
            length, offset = _read_varint(data, offset)
            end = offset + length
            if end > len(data):
                raise ValueError("truncated length-delimited field")
            value, offset = data[offset:end], end
        elif wire_type == 5:
            end = offset + 4
            if end > len(data):
                raise ValueError("truncated fixed32 field")
            value, offset = data[offset:end], end
        else:
            raise ValueError(f"unsupported protobuf wire type {wire_type}")
        fields.setdefault(number, []).append(value)
    return fields


def _first(fields: dict[int, list[Any]], number: int, default: Any = None) -> Any:
    values = fields.get(number)
    return values[0] if values else default


def parse_datagram(data: bytes, sender: tuple[str, int]) -> dict[str, Any] | None:
    """Parse a status datagram, returning None for non-status/malformed input."""

    if len(data) < len(DISCOVERY_MAGIC) + 8 or data[:8] != DISCOVERY_MAGIC:
        return None
    try:
        header_length = struct.unpack_from("<I", data, 8)[0]
        header_start = 12
        header_end = header_start + header_length
        if header_end + 4 > len(data):
            return None
        payload_length = struct.unpack_from("<I", data, header_end)[0]
        payload_start = header_end + 4
        payload_end = payload_start + payload_length
        if payload_end > len(data):
            return None
        header = _parse_fields(data[header_start:header_end])
        if _first(header, 2) != STATUS_MESSAGE:
            return None
        payload = _parse_fields(data[payload_start:payload_end])
    except (struct.error, ValueError, UnicodeError):
        return None

    def string_field(number: int, default: str = "") -> str:
        value = _first(payload, number)
        if not isinstance(value, bytes):
            return default
        return value.decode("utf-8", errors="replace")

    def int_field(number: int, default: int = 0) -> int:
        value = _first(payload, number, default)
        return value if isinstance(value, int) else default

    return {
        "client_id": _first(header, 1, 0),
        "instance_id": _first(header, 3, 0),
        "hostname": string_field(4),
        "address": sender[0],
        "source_port": sender[1],
        "connect_port": int_field(3),
        "ostype": int_field(7),
        "euniverse": int_field(11),
        "games_running": bool(int_field(14)),
    }


def discover(target: str, port: int, timeout: float) -> list[dict[str, Any]]:
    client_id = random.getrandbits(64) or 1
    deadline = time.monotonic() + timeout
    hosts: dict[int, dict[str, Any]] = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if target == "255.255.255.255":
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(min(0.5, max(timeout, 0.01)))
        sock.bind(("0.0.0.0", 0))
        sequence = 0
        while time.monotonic() < deadline:
            sock.sendto(build_discovery_packet(client_id, sequence), (target, port))
            sequence += 1
            receive_until = min(deadline, time.monotonic() + 0.5)
            while time.monotonic() < receive_until:
                try:
                    data, sender = sock.recvfrom(4096)
                except socket.timeout:
                    break
                host = parse_datagram(data, sender)
                if host is not None:
                    hosts[int(host["client_id"])] = host
    return list(hosts.values())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--target", default="255.255.255.255", help="broadcast or Steam host IPv4 address"
    )
    parser.add_argument("--port", type=int, default=27036)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()
    hosts = discover(args.target, args.port, args.timeout)
    if args.json:
        print(json.dumps(hosts, indent=2, sort_keys=True))
    else:
        for host in hosts:
            name = host["hostname"] or "(unnamed host)"
            print(
                f"{name}: {host['address']}:{host['connect_port'] or host['source_port']} "
                f"client_id={host['client_id']} games_running={host['games_running']}"
            )
        if not hosts:
            print("No Steam hosts found")
    return 0 if hosts else 1


if __name__ == "__main__":
    raise SystemExit(main())
