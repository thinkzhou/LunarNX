#!/usr/bin/env python3
"""Deterministic Xbox post-RTP pipeline safety comparison.

This models complete access units after the jitter buffer. It intentionally
separates encoded-worker queueing from the two-frame renderer handoff and UI
presentation. Rejected policies document why display-stage stale-frame drops
and aggressive queue recovery are not enabled. Values are comparative, not
input-to-photon measurements.
"""

from collections import deque
from dataclasses import dataclass
import heapq
import random
import statistics


FPS = 60
FRAME_MS = 1000.0 / FPS
DURATION_FRAMES = 1200
IDR_RETURN_MS = 67.0
RENDERER_CAPACITY = 2


@dataclass(frozen=True)
class QueuePolicy:
    name: str
    max_packets: int
    max_bytes: int
    max_age_ms: float | None
    drop_stale_video: bool


@dataclass(frozen=True)
class Scenario:
    name: str
    network_burst_ms: float = 0.0
    worker_stall_ms: float = 0.0
    ui_stall_ms: float = 0.0


@dataclass
class Result:
    recovery_count: int = 0
    encoded_drops: int = 0
    renderer_drops: int = 0
    sync_drops: int = 0
    presented: int = 0
    latency_ms: list[float] | None = None
    present_gaps_ms: list[float] | None = None

    def __post_init__(self) -> None:
        self.latency_ms = [] if self.latency_ms is None else self.latency_ms
        self.present_gaps_ms = (
            [] if self.present_gaps_ms is None else self.present_gaps_ms)


CURRENT = QueuePolicy("current", 2048, 32 * 1024 * 1024, None, False)
REJECTED_SYNC_DROP = QueuePolicy(
    "reject-sync", 2048, 32 * 1024 * 1024, None, True)
REJECTED_BOUND_50 = QueuePolicy(
    "reject-b50", 3, 8 * 1024 * 1024, 50.0, True)
REJECTED_BOUND_100 = QueuePolicy(
    "reject-b100", 6, 8 * 1024 * 1024, 100.0, True)


def in_one_off_stall(now: float, duration: float, start: float) -> float:
    if duration > 0.0 and start <= now < start + duration:
        return start + duration
    return now


def run(seed: int, scenario: Scenario, bitrate_mbps: int,
        policy: QueuePolicy) -> Result:
    rng = random.Random(seed)
    arrivals: list[tuple[float, int, bool, int, float]] = []
    for sequence in range(DURATION_FRAMES):
        pts = sequence * FRAME_MS
        arrival = max(0.0, pts + rng.uniform(-0.75, 0.75))
        if scenario.network_burst_ms and 3000.0 <= pts < 3070.0:
            arrival += scenario.network_burst_ms
        is_idr = sequence % 120 == 0
        ordinary_bytes = bitrate_mbps * 1_000_000 / 8 / FPS
        size = int(ordinary_bytes *
                   (8.0 if is_idr else rng.uniform(0.65, 1.35)))
        heapq.heappush(arrivals, (arrival, sequence, is_idr, size, pts))

    encoded: deque[tuple[float, int, bool, int, float]] = deque()
    encoded_bytes = 0
    decoded: list[tuple[float, int, float]] = []
    worker_free = 0.0
    waiting_idr = False
    generated_idr: tuple[float, int, bool, int, float] | None = None
    result = Result()
    decode_service_ms = 4.0

    def decode_until(now: float) -> None:
        nonlocal encoded_bytes, worker_free
        while encoded and worker_free <= now and not waiting_idr:
            frame = encoded.popleft()
            encoded_bytes -= frame[3]
            started = max(worker_free, frame[0])
            started = in_one_off_stall(
                started, scenario.worker_stall_ms, 5000.0)
            completed = started + decode_service_ms
            worker_free = completed
            # Rejected experiment: dropping here cannot drain the encoded
            # queue or rebase the A/V clocks, so persistent lag becomes a
            # longer visual freeze instead of a catch-up mechanism.
            if policy.drop_stale_video and completed - frame[4] >= 200.0:
                result.sync_drops += 1
            else:
                decoded.append((completed, frame[1], frame[4]))

    while arrivals or generated_idr is not None:
        if generated_idr is not None and (
                not arrivals or generated_idr[0] <= arrivals[0][0]):
            frame = generated_idr
            generated_idr = None
        else:
            frame = heapq.heappop(arrivals)
        now, sequence, is_idr, size, pts = frame
        decode_until(now)

        if waiting_idr and not is_idr:
            result.encoded_drops += 1
            continue
        if waiting_idr and is_idr:
            waiting_idr = False
            result.encoded_drops += len(encoded)
            encoded.clear()
            encoded_bytes = 0

        oldest_age = now - encoded[0][0] if encoded else 0.0
        age_exceeded = (policy.max_age_ms is not None and encoded and
                        oldest_age >= policy.max_age_ms)
        capacity_exceeded = (
            len(encoded) >= policy.max_packets or
            size > policy.max_bytes - encoded_bytes)
        if age_exceeded or capacity_exceeded:
            result.recovery_count += 1
            result.encoded_drops += len(encoded)
            encoded.clear()
            encoded_bytes = 0
            if is_idr:
                waiting_idr = False
                encoded.append(frame)
                encoded_bytes = size
            else:
                waiting_idr = True
                result.encoded_drops += 1
                generated_idr = (
                    now + IDR_RETURN_MS, sequence, True,
                    max(size, 1024 * 1024), now)
        else:
            encoded.append(frame)
            encoded_bytes += size
        decode_until(now)

    decode_until(float("inf"))
    decoded.sort(key=lambda item: (item[0], item[1]))

    ready: deque[tuple[float, int, float]] = deque()
    decoded_index = 0
    last_present_ms: float | None = None
    for tick in range(DURATION_FRAMES):
        now = tick * FRAME_MS
        if 7000.0 <= now < 7000.0 + scenario.ui_stall_ms:
            continue
        while decoded_index < len(decoded) and decoded[decoded_index][0] <= now:
            if len(ready) == RENDERER_CAPACITY:
                ready.popleft()
                result.renderer_drops += 1
            ready.append(decoded[decoded_index])
            decoded_index += 1
        if not ready:
            continue
        _, _, pts = ready.popleft()
        result.presented += 1
        result.latency_ms.append(now - pts)
        if last_present_ms is not None:
            result.present_gaps_ms.append(now - last_present_ms)
        last_present_ms = now
    return result


def p95(values: list[float]) -> float:
    if not values:
        return 0.0
    return statistics.quantiles(values, n=20, method="inclusive")[18]


def aggregate(results: list[Result]) -> dict[str, float]:
    latency = [value for result in results for value in result.latency_ms]
    gaps = [value for result in results for value in result.present_gaps_ms]
    return {
        "recoveries": sum(result.recovery_count for result in results) /
                      len(results),
        "encoded_drop": sum(result.encoded_drops for result in results) /
                        len(results),
        "renderer_drop": sum(result.renderer_drops for result in results) /
                         len(results),
        "sync_drop": sum(result.sync_drops for result in results) /
                     len(results),
        "presented": sum(result.presented for result in results) /
                     len(results),
        "latency_p95": p95(latency),
        "latency_max": max(latency) if latency else 0.0,
        "freeze_max": max(gaps) if gaps else 0.0,
        "stale_over_100": (100.0 * sum(value > 100.0 for value in latency) /
                           len(latency)) if latency else 0.0,
    }


def measure(scenario: Scenario, policy: QueuePolicy) -> dict[str, float]:
    return aggregate([
        run(seed, scenario, bitrate, policy)
        for seed in range(10)
        for bitrate in (20, 30)
    ])


def main() -> None:
    scenarios = [
        Scenario("clean"),
        Scenario("network_burst_70", network_burst_ms=70.0),
        Scenario("worker_stall_25", worker_stall_ms=25.0),
        Scenario("worker_stall_50", worker_stall_ms=50.0),
        Scenario("worker_stall_100", worker_stall_ms=100.0),
        Scenario("worker_stall_250", worker_stall_ms=250.0),
        Scenario("worker_stall_1000", worker_stall_ms=1000.0),
        Scenario("ui_stall_100", ui_stall_ms=100.0),
        Scenario("ui_stall_250", ui_stall_ms=250.0),
        Scenario("combined", 70.0, 100.0, 100.0),
    ]
    policies = (CURRENT, REJECTED_SYNC_DROP,
                REJECTED_BOUND_50, REJECTED_BOUND_100)
    measurements: dict[tuple[str, str], dict[str, float]] = {}
    print("Xbox local post-RTP pipeline simulation (10 seeds x 20/30 Mbps)")
    print("scenario             policy    recov encDrop syncDrop rendDrop latP95"
          " latMax freezeMax stale>100%")
    for scenario in scenarios:
        for policy in policies:
            value = measure(scenario, policy)
            measurements[(scenario.name, policy.name)] = value
            print(f"{scenario.name:<20} {policy.name:<8} "
                  f"{value['recoveries']:5.1f} {value['encoded_drop']:7.1f} "
                  f"{value['sync_drop']:8.1f} "
                  f"{value['renderer_drop']:8.1f} "
                  f"{value['latency_p95']:6.1f} {value['latency_max']:6.1f} "
                  f"{value['freeze_max']:9.1f} "
                  f"{value['stale_over_100']:10.2f}")

    clean_current = measurements[("clean", "current")]
    clean_bound100 = measurements[("clean", "reject-b100")]
    stall_current = measurements[("worker_stall_1000", "current")]
    stall_syncfix = measurements[("worker_stall_1000", "reject-sync")]
    stall_bound100 = measurements[("worker_stall_1000", "reject-b100")]
    assert clean_current["recoveries"] == 0
    assert clean_bound100["recoveries"] == 0
    assert clean_bound100["latency_p95"] <= clean_current["latency_p95"] + FRAME_MS
    assert stall_current["sync_drop"] == 0
    assert stall_current["latency_max"] > 900.0
    assert stall_syncfix["sync_drop"] > 0
    assert stall_syncfix["latency_max"] <= 200.0
    assert stall_syncfix["freeze_max"] > stall_current["freeze_max"]
    assert stall_bound100["recoveries"] > 0
    assert stall_bound100["encoded_drop"] > 0
    assert stall_bound100["freeze_max"] >= stall_current["freeze_max"]
    assert stall_bound100["freeze_max"] < stall_syncfix["freeze_max"]
    assert stall_bound100["stale_over_100"] < stall_current["stale_over_100"]
    print("Current 32 MiB queue time capacity at 10/20/30 Mbps: "
          "26.84/13.42/8.95 seconds (packet cap: 34.13 seconds at 60 fps)")
    print("All Xbox local pipeline safety checks passed")


if __name__ == "__main__":
    main()
