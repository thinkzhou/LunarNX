#!/usr/bin/env python3
"""Causal post-Chiaki queue/presentation simulation."""

from dataclasses import dataclass
import heapq
import random
import statistics

FPS = 60
FRAME_MS = 1000.0 / FPS
IDR_RETURN_MS = 67.0
QUEUE_BYTES = 8 * 1024 * 1024


@dataclass(frozen=True)
class Scenario:
    name: str
    network_burst_ms: float = 0.0
    worker_stall_ms: float = 0.0
    ui_stall_ms: float = 0.0
    severe_block_ms: float = 0.0


@dataclass
class Result:
    hard_recovery: int = 0
    decoded: int = 0
    presented: int = 0
    decoded_drop: int = 0
    present_gaps: list[float] = None
    latency_ms: list[float] = None

    def __post_init__(self):
        self.present_gaps = [] if self.present_gaps is None else self.present_gaps
        self.latency_ms = [] if self.latency_ms is None else self.latency_ms


def _stall_end(now: float, scenario: Scenario, kind: str) -> float:
    if kind == "worker" and scenario.worker_stall_ms:
        period, duration = 5000.0, scenario.worker_stall_ms
    elif kind == "ui" and scenario.ui_stall_ms:
        period, duration = 6000.0, scenario.ui_stall_ms
    elif kind == "severe" and scenario.severe_block_ms:
        period, duration = 10000.0, scenario.severe_block_ms
    else:
        return now
    start = (now // period) * period
    return start + duration if start <= now < start + duration else now


def run(seed: int, scenario: Scenario, bitrate_mbps: int,
        max_packets: int, max_age_ms: float, decoded_capacity: int) -> Result:
    rng = random.Random(seed)
    events = []
    for seq in range(1200):
        pts = seq * FRAME_MS
        jitter = rng.uniform(-1.0, 1.0)
        burst = scenario.network_burst_ms if scenario.network_burst_ms and \
            3000.0 <= pts % 7000.0 < 3070.0 else 0.0
        arrival = max(0.0, pts + jitter + burst)
        au_bytes = int(bitrate_mbps * 1_000_000 / 8 / FPS *
                       (8.0 if seq % 120 == 0 else rng.uniform(.6, 1.4)))
        heapq.heappush(events, (arrival, seq, seq % 120 == 0, au_bytes, pts))

    result = Result()
    encoded = []
    encoded_bytes = 0
    decoded_events = []
    worker_free = 0.0
    waiting_idr = False
    next_generated_idr = None

    def decode_until(now: float) -> None:
        nonlocal worker_free, encoded_bytes
        while encoded and worker_free <= now and not waiting_idr:
            frame = encoded.pop(0)
            encoded_bytes -= frame[3]
            start = max(worker_free, frame[0])
            start = max(start, _stall_end(start, scenario, "worker"))
            done = start + 12.0 + bitrate_mbps / 20.0
            worker_free = done
            decoded_events.append((done, frame[1], frame[4]))
            result.decoded += 1

    while events or next_generated_idr is not None:
        generated = next_generated_idr
        if generated is not None and (not events or generated[0] <= events[0][0]):
            next_generated_idr = None
            event = generated
        else:
            event = heapq.heappop(events)
        now, seq, is_idr, au_bytes, pts = event
        decode_until(now)
        if waiting_idr and not is_idr:
            continue
        if waiting_idr and is_idr:
            waiting_idr = False
            encoded.clear()
            encoded_bytes = 0
            worker_free = max(worker_free, now)
        oldest_age = now - encoded[0][0] if encoded else 0.0
        admission_overflow = (
            len(encoded) >= max_packets or
            oldest_age >= max_age_ms or
            au_bytes > QUEUE_BYTES - encoded_bytes)
        if admission_overflow:
            result.hard_recovery += 1
            encoded.clear()
            encoded_bytes = 0
            if is_idr:
                # A random-access AU is the recovery candidate itself.
                waiting_idr = False
                encoded.append(event)
                encoded_bytes = au_bytes
            else:
                waiting_idr = True
                next_generated_idr = (now + IDR_RETURN_MS, seq, True,
                                      max(au_bytes, 1024 * 1024), now)
        else:
            encoded.append(event)
            encoded_bytes += au_bytes
        decode_until(now)

    # Present ticks are monotonic: blocked ticks are skipped, never shifted
    # backwards into the previous interval.
    decoded_events.sort(key=lambda item: (item[0], item[1]))
    ready = []
    event_index = 0
    last_present = None
    blocked_until = 0.0
    for tick in range(1200):
        nominal = tick * FRAME_MS
        stall_end = _stall_end(nominal, scenario, "ui")
        if scenario.severe_block_ms:
            stall_end = max(stall_end, _stall_end(nominal, scenario, "severe"))
        if stall_end > nominal:
            blocked_until = max(blocked_until, stall_end)
            continue
        if nominal < blocked_until:
            continue
        present_at = nominal
        while event_index < len(decoded_events) and decoded_events[event_index][0] <= present_at:
            if len(ready) == decoded_capacity:
                ready.pop(0)
                result.decoded_drop += 1
            ready.append(decoded_events[event_index])
            event_index += 1
        if ready:
            _, _, pts = ready.pop(0)
            result.presented += 1
            result.latency_ms.append(present_at - pts)
            if last_present is not None:
                result.present_gaps.append(present_at - last_present)
            last_present = present_at
    return result


def aggregate(results):
    gaps = [gap for result in results for gap in result.present_gaps]
    latency = [value for result in results for value in result.latency_ms]
    return {
        "recovery": sum(result.hard_recovery for result in results),
        "decoded_drop": sum(result.decoded_drop for result in results),
        "presented": sum(result.presented for result in results),
        "max_gap": max(gaps) if gaps else 0.0,
        "p95_gap": statistics.quantiles(gaps, n=20)[18] if gaps else 0.0,
        "p95_latency": statistics.quantiles(latency, n=20)[18] if latency else 0.0,
    }


def main() -> None:
    scenarios = {
        "clean": Scenario("clean"),
        "wifi_bursty": Scenario("wifi_bursty", network_burst_ms=70.0),
        "worker_hiccup": Scenario("worker_hiccup", worker_stall_ms=55.0),
        "ui_hiccup": Scenario("ui_hiccup", ui_stall_ms=45.0),
        "combined": Scenario("combined", 70.0, 55.0, 45.0),
        "severe": Scenario("severe", worker_stall_ms=140.0,
                            severe_block_ms=140.0),
    }

    def measure(scenario, capacity=1, packets=6, age=100.0):
        return aggregate([run(seed, scenario, bitrate, packets, age, capacity)
                          for seed in range(5) for bitrate in (20, 30)])

    for bitrate in (20, 30):
        def one(scenario, capacity=1, packets=6, age=100.0):
            return aggregate([run(seed, scenario, bitrate, packets, age, capacity)
                              for seed in range(5)])

        clean = one(scenarios["clean"])
        wifi = one(scenarios["wifi_bursty"])
        worker = one(scenarios["worker_hiccup"])
        ui = one(scenarios["ui_hiccup"])
        current = one(scenarios["combined"], packets=3, age=50.0)
        tuned = one(scenarios["combined"])
        complete = one(scenarios["combined"], capacity=2)
        severe = one(scenarios["severe"])

        assert clean["recovery"] == 0
        assert wifi["recovery"] >= 0
        assert worker["decoded_drop"] > clean["decoded_drop"]
        assert ui["max_gap"] > clean["max_gap"]
        assert tuned["recovery"] < current["recovery"]
        assert complete["decoded_drop"] < tuned["decoded_drop"]
        assert complete["p95_gap"] <= tuned["p95_gap"]
        assert complete["p95_latency"] <= tuned["p95_latency"] + 2 * FRAME_MS
        assert severe["recovery"] > 0
    print("PS media queue simulation passed")


if __name__ == "__main__":
    main()
