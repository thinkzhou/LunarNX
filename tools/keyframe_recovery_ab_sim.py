#!/usr/bin/env python3
"""Deterministic green8/G9 vs current recovery-PLI cadence simulation."""

from __future__ import annotations

from dataclasses import dataclass
import random
import statistics


TRIALS = 2000
HORIZON_MS = 6000


@dataclass(frozen=True)
class Scenario:
    name: str
    rtt_ms: int
    pli_loss: float
    sender_idr_ms: int


@dataclass(frozen=True)
class Result:
    average_ms: float
    p95_ms: int
    p99_ms: int
    worst_ms: int
    timeout_pct: float
    requests: float


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    return ordered[int((len(ordered) - 1) * fraction)]


def request_times(policy: str, old_phase_ms: int) -> list[int]:
    if policy == "checkpoint":
        first = 1000 - old_phase_ms
        return list(range(first, HORIZON_MS + 1, 1000))

    # Current VideoRecoveryRequestPolicy: immediate, +300 ms, +500 ms, then
    # 1 Hz. Times are relative to the first observed recovery condition.
    times = [0, 300, 800]
    while times[-1] + 1000 <= HORIZON_MS:
        times.append(times[-1] + 1000)
    return times


def run_trial(policy: str, scenario: Scenario, seed: int) -> tuple[int, int]:
    rng = random.Random(seed)
    phase = rng.randrange(1000)
    earliest_idr = HORIZON_MS + 1
    requests = 0
    for request_ms in request_times(policy, phase):
        if request_ms >= earliest_idr:
            break
        requests += 1
        if rng.random() < scenario.pli_loss:
            continue
        arrival_ms = request_ms + scenario.rtt_ms + scenario.sender_idr_ms
        earliest_idr = min(earliest_idr, arrival_ms)
    return min(earliest_idr, HORIZON_MS), requests


def aggregate(policy: str, scenario: Scenario) -> Result:
    samples = [
        run_trial(policy, scenario, seed)
        for seed in range(TRIALS)
    ]
    times = [sample[0] for sample in samples]
    return Result(
        average_ms=statistics.fmean(times),
        p95_ms=percentile(times, 0.95),
        p99_ms=percentile(times, 0.99),
        worst_ms=max(times),
        timeout_pct=sum(value >= HORIZON_MS for value in times) * 100 / TRIALS,
        requests=statistics.fmean(sample[1] for sample in samples),
    )


def main() -> None:
    scenarios = [
        Scenario("lan_clean", 20, 0.00, 50),
        Scenario("cloud_clean", 75, 0.00, 80),
        Scenario("cloud_pli_loss10", 75, 0.10, 80),
        Scenario("cloud_pli_loss30", 75, 0.30, 80),
        Scenario("cloud_rtt180_loss10", 180, 0.10, 100),
        Scenario("slow_sender_idr", 75, 0.10, 600),
    ]
    print(f"Recovery PLI A/B ({TRIALS} deterministic trials)")
    print("checkpoint=fixed 1 Hz; new=immediate/300/800 ms then 1 Hz")
    print("scenario              policy      avg  p95  p99 worst timeout% reqAvg")
    results: dict[tuple[str, str], Result] = {}
    for scenario in scenarios:
        for policy in ("checkpoint", "new"):
            result = aggregate(policy, scenario)
            results[(scenario.name, policy)] = result
            print(
                f"{scenario.name:<21} {policy:<10} "
                f"{result.average_ms:5.0f} {result.p95_ms:4d} "
                f"{result.p99_ms:4d} {result.worst_ms:5d} "
                f"{result.timeout_pct:8.2f} {result.requests:6.2f}"
            )

    for scenario in scenarios:
        old = results[(scenario.name, "checkpoint")]
        new = results[(scenario.name, "new")]
        assert new.average_ms < old.average_ms
        assert new.p95_ms < old.p95_ms
        assert new.timeout_pct <= old.timeout_pct
        assert new.requests <= old.requests + 1.5
    assert results[("cloud_pli_loss30", "new")].p95_ms <= 1000
    assert results[("cloud_rtt180_loss10", "new")].p95_ms <= 580
    print("All recovery PLI simulation checks passed")


if __name__ == "__main__":
    main()
