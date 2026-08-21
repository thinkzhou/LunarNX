#!/usr/bin/env python3
"""Deterministic post-Chiaki queue/presentation simulation.

This deliberately models complete access units, not Takion packets.  It is a
cheap regression for the queue and renderer policies used by ps_media_replay.
"""

from dataclasses import dataclass
import random


FPS = 60
FRAME_MS = 1000.0 / FPS
IDR_RETURN_MS = 67.0


@dataclass
class Result:
    recovery: int = 0
    presented: int = 0
    long_gaps: int = 0


def run(seed: int, max_packets: int, max_age: float, decoded_capacity: int) -> Result:
    rng = random.Random(seed)
    encoded = []
    decoded = []
    decoded_events = []
    result = Result()
    next_idr = -1.0
    last_present = None
    next_decode = 0.0

    for index in range(1200):
        nominal = index * FRAME_MS
        burst = 70.0 if 3000.0 <= nominal % 7000.0 < 7070.0 else 0.0
        arrival = nominal + burst + rng.uniform(-1.0, 1.0)
        encoded.append(arrival)

        # Drain at one frame per display interval.  A burst therefore creates
        # a real AU backlog instead of being silently handled as a faster
        # decoder.
        now = arrival
        if now >= next_idr:
            next_idr = -1.0
        while encoded and next_decode <= now and next_idr < 0:
            encoded.pop(0)
            decoded_events.append((next_decode, index))
            next_decode += FRAME_MS
            if 5000.0 <= next_decode % 10000.0 < 5055.0:
                next_decode += 55.0
        if next_idr < 0 and encoded and (len(encoded) >= max_packets or
                                         now - encoded[0] >= max_age):
            result.recovery += 1
            encoded.clear()
            next_idr = now + IDR_RETURN_MS
            next_decode = max(next_decode, next_idr)
            continue
        if next_idr >= 0:
            continue

    # Replay decoded arrivals against the UI's deterministic present clock.
    event_index = 0
    for tick in range(1200):
        present_at = tick * FRAME_MS
        if 8000.0 <= present_at % 12000.0 < 8045.0:
            present_at += 45.0
        while event_index < len(decoded_events) and decoded_events[event_index][0] <= present_at:
            if len(decoded) == decoded_capacity:
                decoded.pop(0)
            decoded.append(decoded_events[event_index])
            event_index += 1
        if decoded:
            decoded.pop(0)
            result.presented += 1
            if last_present is not None and present_at - last_present > 33.4:
                result.long_gaps += 1
            last_present = present_at

    return result


def main() -> None:
    current = [run(seed, 3, 50.0, 1) for seed in range(20)]
    tuned = [run(seed, 6, 100.0, 1) for seed in range(20)]
    complete = [run(seed, 6, 100.0, 2) for seed in range(20)]

    # The exact counts are intentionally not part of the contract.  These
    # inequalities protect the behavior the offline study is meant to select.
    assert sum(x.recovery for x in tuned) < sum(x.recovery for x in current)
    assert sum(x.recovery for x in complete) == sum(x.recovery for x in tuned)
    assert sum(x.presented for x in complete) >= sum(x.presented for x in tuned)
    print("PS media queue simulation passed")


if __name__ == "__main__":
    main()
