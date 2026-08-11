#!/usr/bin/env python3
import argparse
import json
import os
import socket
import struct
import time
from collections import Counter
from pathlib import Path


MAGIC = 0x4C4E5855


def log(message):
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"{timestamp} {message}", flush=True)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Echo LunarNX Ryubing UDP probe packets")
    parser.add_argument("--listen-address", required=True)
    parser.add_argument("--port", type=int, default=48000)
    parser.add_argument("--idle-timeout", type=float, default=10.0)
    parser.add_argument("--ready-file", type=Path)
    args = parser.parse_args()
    socket.inet_aton(args.listen_address)
    if not 0 <= args.port <= 65535:
        parser.error("port must be in 0..65535")
    return args


def main():
    args = parse_args()
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind((args.listen_address, args.port))
    server.settimeout(0.25)
    port = server.getsockname()[1]
    if args.ready_file:
        args.ready_file.write_text(json.dumps({"port": port}) + "\n")
    log(f"UDP echo probe listening on {args.listen_address}:{port}")
    counts = Counter()
    unique = {1: set(), 2: set()}
    malformed = 0
    last_packet = time.monotonic()
    try:
        while time.monotonic() - last_packet < args.idle_timeout:
            try:
                packet, source = server.recvfrom(65535)
            except socket.timeout:
                continue
            last_packet = time.monotonic()
            if len(packet) < 16:
                malformed += 1
                continue
            magic, phase, sequence, total = struct.unpack("!IIII", packet[:16])
            if magic != MAGIC or phase not in unique or sequence >= total:
                malformed += 1
                continue
            counts[phase] += 1
            unique[phase].add(sequence)
            server.sendto(packet, source)
    except KeyboardInterrupt:
        pass
    finally:
        log("UDP echo probe summary "
            f"sequential_packets={counts[1]} sequential_unique={len(unique[1])} "
            f"burst_packets={counts[2]} burst_unique={len(unique[2])} "
            f"malformed={malformed}")
        server.close()
        if args.ready_file:
            try:
                os.unlink(args.ready_file)
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    main()
