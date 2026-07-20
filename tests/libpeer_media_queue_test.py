#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("lib/libpeer/src/peer_connection.c").read_text()

    require("drop RTP while media disabled" not in source,
            "Startup RTP must be queued until the media pipeline is ready")
    require("!pc->media_enabled || budget <= 0" in source,
            "Queued RTP must not be decoded before media is enabled")
    require("peer_connection_enqueue_rtp(pc, pc->agent_buf, pc->agent_ret)" in source,
            "Incoming RTP must use the bounded startup queue")

    print("libpeer media queue tests passed")


if __name__ == "__main__":
    main()
