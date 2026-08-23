#!/usr/bin/env python3
"""Deterministic RTP network-policy benchmark for Xbox video.

The benchmark intentionally does not model decoder or renderer stalls. It
models the network side of the Xbox path: packet delay, jitter, loss, burst
loss, reordering, NACK retransmission, and the frame deadline used by the
jitter buffer. Results measure network/RTP-frame delay, not controller-to-
photon latency.

`current-code` follows the current Home/Cloud hold and missing-packet
deadline formulas in PeerManager and VideoRtpJitterBuffer.
`target-quality-gap-budget` models the new Good/Fair/Poor network-quality
tiers and keeps the ordinary frame hold unchanged while giving a missing
frame a quality-appropriate bounded repair budget.
"""

from __future__ import annotations

import argparse
import csv
import math
import random
import statistics
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


FPS = 60.0
FRAME_MS = 1000.0 / FPS
PACKETS_PER_FRAME = 8
PACKET_SPAN_MS = 2.0
GOP_FRAMES = 120
MAX_NACK_ROUNDS = 3


@dataclass(frozen=True)
class Scenario:
    name: str
    profile: str
    rtt_ms: float
    delay_jitter_ms: float = 0.0
    random_loss: float = 0.0
    burst_loss_packets: int = 0
    burst_count: int = 0
    reorder_ms: float = 0.0
    retransmit_loss: float = 0.0
    retransmit_extra_ms: float = 0.0
    duration_ms: float = 5000.0
    seeds: int = 5


@dataclass(frozen=True)
class Policy:
    name: str
    profile: str


@dataclass(frozen=True)
class Packet:
    frame: int
    packet: int
    arrival_ms: float


@dataclass
class RunStats:
    policy: str
    profile: str
    scenario: str
    seed: int
    frames: int = 0
    presented_frames: int = 0
    incomplete_frames: int = 0
    corrupt_keyframes: int = 0
    missing_packets: int = 0
    recovered_packets: int = 0
    late_packets: int = 0
    nack_packets: int = 0
    retransmit_attempts: int = 0
    retransmit_losses: int = 0
    repair_wait_samples: list[float] = field(default_factory=list)
    frame_delay_samples: list[float] = field(default_factory=list)


def percentile(values: Iterable[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = min(len(ordered) - 1, int(math.ceil(fraction * len(ordered))) - 1)
    return ordered[max(0, index)]


def policies(profile: str) -> list[Policy]:
    return [
        Policy("current-code", profile),
        Policy("target-quality-gap-budget", profile),
    ]


def scenarios() -> list[Scenario]:
    result: list[Scenario] = []
    for profile in ("home", "cloud"):
        base_rtt = 3.0 if profile == "home" else 40.0
        result.extend(
            [
                Scenario(f"{profile}-clean-rtt-{int(base_rtt)}", profile, base_rtt),
                Scenario(f"{profile}-rtt-10", profile, 10.0, delay_jitter_ms=2.0),
                Scenario(f"{profile}-rtt-30", profile, 30.0, delay_jitter_ms=5.0),
                Scenario(f"{profile}-rtt-80", profile, 80.0, delay_jitter_ms=10.0),
                Scenario(
                    f"{profile}-rtt-80-loss-1pct",
                    profile,
                    80.0,
                    delay_jitter_ms=10.0,
                    random_loss=0.01,
                ),
                Scenario(
                    f"{profile}-rtt-80-late-retransmit",
                    profile,
                    80.0,
                    delay_jitter_ms=8.0,
                    random_loss=0.01,
                    retransmit_extra_ms=20.0,
                ),
                Scenario(f"{profile}-rtt-150", profile, 150.0, delay_jitter_ms=20.0),
                Scenario(
                    f"{profile}-rtt-150-late-retransmit",
                    profile,
                    150.0,
                    delay_jitter_ms=20.0,
                    random_loss=0.01,
                    retransmit_extra_ms=30.0,
                ),
                Scenario(f"{profile}-rtt-250", profile, 250.0, delay_jitter_ms=30.0),
                Scenario(
                    f"{profile}-random-loss-0.1pct",
                    profile,
                    base_rtt,
                    random_loss=0.001,
                ),
                Scenario(
                    f"{profile}-random-loss-1pct",
                    profile,
                    base_rtt,
                    random_loss=0.01,
                ),
                Scenario(
                    f"{profile}-random-loss-3pct",
                    profile,
                    base_rtt,
                    random_loss=0.03,
                ),
                Scenario(
                    f"{profile}-burst-5",
                    profile,
                    base_rtt,
                    burst_loss_packets=5,
                    burst_count=3,
                ),
                Scenario(
                    f"{profile}-burst-20",
                    profile,
                    base_rtt,
                    burst_loss_packets=20,
                    burst_count=3,
                ),
                Scenario(
                    f"{profile}-burst-50",
                    profile,
                    base_rtt,
                    burst_loss_packets=50,
                    burst_count=3,
                ),
                Scenario(
                    f"{profile}-reorder-15",
                    profile,
                    base_rtt,
                    delay_jitter_ms=4.0,
                    reorder_ms=15.0,
                ),
                Scenario(
                    f"{profile}-reorder-50",
                    profile,
                    base_rtt,
                    delay_jitter_ms=12.0,
                    reorder_ms=50.0,
                ),
                Scenario(
                    f"{profile}-lossy-retransmit",
                    profile,
                    base_rtt,
                    random_loss=0.01,
                    retransmit_loss=0.25,
                ),
                Scenario(
                    f"{profile}-combined-network-stress",
                    profile,
                    30.0 if profile == "home" else 80.0,
                    delay_jitter_ms=15.0,
                    random_loss=0.01,
                    burst_loss_packets=12,
                    burst_count=2,
                    reorder_ms=30.0,
                    retransmit_loss=0.15,
                ),
            ]
        )
    return result


def current_hold_ms(profile: str, rtt_ms: float) -> float:
    if profile == "home":
        return min(48.0, max(24.0, 24.0 + rtt_ms / 4.0))
    return min(180.0, max(80.0, rtt_ms + 40.0))


def current_missing_hold_ms(profile: str, rtt_ms: float) -> float:
    hold = current_hold_ms(profile, rtt_ms)
    if rtt_ms >= 150.0:
        return max(hold, min(500.0, rtt_ms + hold + 20.0))
    # This is the current kDefaultHoldMs ceiling in
    # VideoRtpJitterBuffer::missingPacketHoldMs().
    return max(hold, min(60.0, rtt_ms + hold + 10.0))


def target_missing_hold_ms(scenario: Scenario) -> float:
    profile = scenario.profile
    rtt_ms = scenario.rtt_ms
    jitter_ms = scenario.delay_jitter_ms
    ordinary = current_hold_ms(profile, rtt_ms)
    good = rtt_ms <= 20 and jitter_ms <= 4 and scenario.random_loss < 0.002 and scenario.burst_loss_packets <= 2
    fair = rtt_ms <= 50 and jitter_ms <= 20 and scenario.random_loss < 0.01 and scenario.burst_loss_packets <= 5
    if good:
        return max(ordinary, min(32.0 if profile == "home" else 120.0, rtt_ms + 8.0))
    if fair:
        return max(ordinary, min(60.0 if profile == "home" else 180.0, rtt_ms + (20.0 if profile == "home" else 40.0)))
    return max(ordinary, min(180.0 if profile == "home" else 300.0, rtt_ms + ordinary + 20.0))


def nack_retry_interval_ms(rtt_ms: float) -> float:
    return max(20.0, min(60.0, rtt_ms / 2.0))


def make_packets(scenario: Scenario, seed: int) -> tuple[list[Packet], set[tuple[int, int]]]:
    rng = random.Random(seed)
    frame_count = int(scenario.duration_ms / FRAME_MS)
    total_packets = frame_count * PACKETS_PER_FRAME
    burst_targets: set[int] = set()
    for _ in range(scenario.burst_count):
        if scenario.burst_loss_packets:
            start = rng.randrange(max(1, total_packets - scenario.burst_loss_packets))
            burst_targets.update(range(start, start + scenario.burst_loss_packets))

    packets: list[Packet] = []
    lost: set[tuple[int, int]] = set()
    serial = 0
    one_way = scenario.rtt_ms / 2.0
    for frame in range(frame_count):
        frame_start = frame * FRAME_MS
        for packet_index in range(PACKETS_PER_FRAME):
            arrival = frame_start + packet_index * PACKET_SPAN_MS / (PACKETS_PER_FRAME - 1)
            arrival += one_way
            if scenario.delay_jitter_ms:
                arrival += max(0.0, rng.gauss(0.0, scenario.delay_jitter_ms))
            if scenario.reorder_ms and rng.random() < 0.25:
                arrival += rng.uniform(0.0, scenario.reorder_ms)
            lost_packet = serial in burst_targets or rng.random() < scenario.random_loss
            if lost_packet:
                lost.add((frame, packet_index))
            else:
                packets.append(Packet(frame, packet_index, arrival))
            serial += 1
    packets.sort(key=lambda packet: packet.arrival_ms)
    return packets, lost


def run_once(scenario: Scenario, policy: Policy, seed: int) -> RunStats:
    stats = RunStats(policy.name, policy.profile, scenario.name, seed)
    packets, lost = make_packets(scenario, seed)
    by_frame: dict[int, list[Packet]] = {}
    for packet in packets:
        by_frame.setdefault(packet.frame, []).append(packet)

    frame_count = int(scenario.duration_ms / FRAME_MS)
    stats.frames = frame_count
    hold = current_hold_ms(scenario.profile, scenario.rtt_ms)
    missing_hold = (
        current_missing_hold_ms(scenario.profile, scenario.rtt_ms)
        if policy.name == "current-code"
        else target_missing_hold_ms(scenario)
    )
    retry_interval = nack_retry_interval_ms(scenario.rtt_ms)
    rng = random.Random(seed + 20_261)

    for frame in range(frame_count):
        received = by_frame.get(frame, [])
        received_packets = {packet.packet: packet.arrival_ms for packet in received}
        missing = [index for index in range(PACKETS_PER_FRAME) if index not in received_packets]
        stats.missing_packets += len(missing)
        frame_start = frame * FRAME_MS
        last_arrival = max(
            (packet.arrival_ms for packet in received),
            default=frame_start + scenario.rtt_ms / 2.0,
        )
        detection = last_arrival
        deadline = detection + missing_hold
        repaired = 0

        if missing:
            for nack_round in range(MAX_NACK_ROUNDS):
                if not missing:
                    break
                nack_time = detection + nack_round * retry_interval
                for packet_index in list(missing):
                    stats.nack_packets += 1
                    stats.retransmit_attempts += 1
                    if rng.random() < scenario.retransmit_loss:
                        stats.retransmit_losses += 1
                        continue
                    retransmit_arrival = (
                        nack_time
                        + scenario.rtt_ms
                        + scenario.retransmit_extra_ms
                    )
                    if scenario.delay_jitter_ms:
                        retransmit_arrival += max(
                            0.0, rng.gauss(0.0, scenario.delay_jitter_ms)
                        )
                    if scenario.reorder_ms and rng.random() < 0.15:
                        retransmit_arrival += rng.uniform(0.0, scenario.reorder_ms)
                    if retransmit_arrival <= deadline:
                        received_packets[packet_index] = retransmit_arrival
                        missing.remove(packet_index)
                        repaired += 1

        if missing:
            stats.incomplete_frames += 1
            if frame % GOP_FRAMES == 0:
                stats.corrupt_keyframes += 1
            continue

        if repaired:
            stats.recovered_packets += repaired
            stats.repair_wait_samples.append(
                max(received_packets.values()) - last_arrival
            )
        emit_time = max(received_packets.values())
        original_late = sum(
            arrival > detection + hold
            for arrival in received_packets.values()
        )
        stats.late_packets += int(original_late)
        stats.presented_frames += 1
        stats.frame_delay_samples.append(emit_time - frame_start)

    return stats


def aggregate(runs: list[RunStats]) -> list[dict[str, object]]:
    groups: dict[tuple[str, str, str], list[RunStats]] = {}
    for run in runs:
        groups.setdefault((run.policy, run.profile, run.scenario), []).append(run)
    rows: list[dict[str, object]] = []
    for (policy, profile, scenario), items in sorted(groups.items()):
        delays = [value for item in items for value in item.frame_delay_samples]
        repair_waits = [value for item in items for value in item.repair_wait_samples]
        frames = statistics.mean(item.frames for item in items)
        rows.append(
            {
                "policy": policy,
                "profile": profile,
                "scenario": scenario,
                "runs": len(items),
                "presented_pct": round(
                    100.0 * statistics.mean(item.presented_frames for item in items) / frames,
                    2,
                ),
                "incomplete_pct": round(
                    100.0 * statistics.mean(item.incomplete_frames for item in items) / frames,
                    2,
                ),
                "p50_frame_delay_ms": round(percentile(delays, 0.50), 2),
                "p95_frame_delay_ms": round(percentile(delays, 0.95), 2),
                "p99_frame_delay_ms": round(percentile(delays, 0.99), 2),
                "max_frame_delay_ms": round(max(delays, default=0.0), 2),
                "p95_repair_wait_ms": round(percentile(repair_waits, 0.95), 2),
                "missing_packets": round(statistics.mean(item.missing_packets for item in items), 1),
                "recovered_packets": round(statistics.mean(item.recovered_packets for item in items), 1),
                "late_packets": round(statistics.mean(item.late_packets for item in items), 1),
                "nack_packets": round(statistics.mean(item.nack_packets for item in items), 1),
                "retransmit_losses": round(statistics.mean(item.retransmit_losses for item in items), 1),
                "corrupt_keyframes": round(statistics.mean(item.corrupt_keyframes for item in items), 1),
            }
        )
    return rows


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def write_markdown(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    by_key = {(row["profile"], row["scenario"], row["policy"]): row for row in rows}
    selected = [
        "home-clean-rtt-3",
        "home-rtt-80",
        "home-rtt-80-loss-1pct",
        "home-rtt-80-late-retransmit",
        "home-random-loss-1pct",
        "home-burst-20",
        "home-lossy-retransmit",
        "home-combined-network-stress",
        "cloud-clean-rtt-40",
        "cloud-rtt-150",
        "cloud-rtt-150-late-retransmit",
        "cloud-random-loss-1pct",
        "cloud-burst-20",
        "cloud-lossy-retransmit",
        "cloud-combined-network-stress",
    ]
    lines = [
        "# Xbox RTP network policy benchmark",
        "",
        "This benchmark models packet delay, delay jitter, random/burst loss, reordering, NACK retransmission, and frame deadlines. It does not model decoder or renderer stalls.",
        "",
        "`current-code` follows the current PeerManager/VideoRtpJitterBuffer formulas. `target-quality-gap-budget` models the new Good/Fair/Poor tiers and keeps ordinary Home/Cloud hold unchanged while giving incomplete frames a quality-appropriate bounded repair budget.",
        "",
        "## Selected comparison",
        "",
        "`Δ p95` is target minus current. Negative means the target has lower network-to-frame delay.",
        "",
        "| Profile | Scenario | Current p95 | Target p95 | Δ p95 | Current incomplete % | Target incomplete % | Current repair p95 | Target repair p95 |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for scenario in selected:
        profile = scenario.split("-", 1)[0]
        current = by_key[(profile, scenario, "current-code")]
        target = by_key[(profile, scenario, "target-quality-gap-budget")]
        delta = round(
            float(target["p95_frame_delay_ms"])
            - float(current["p95_frame_delay_ms"]),
            2,
        )
        lines.append(
            "| {profile} | {scenario} | {current} | {target} | {delta} | {current_incomplete} | {target_incomplete} | {current_repair} | {target_repair} |".format(
                profile=profile,
                scenario=scenario,
                current=current["p95_frame_delay_ms"],
                target=target["p95_frame_delay_ms"],
                delta=delta,
                current_incomplete=current["incomplete_pct"],
                target_incomplete=target["incomplete_pct"],
                current_repair=current["p95_repair_wait_ms"],
                target_repair=target["p95_repair_wait_ms"],
            )
        )
    lines.extend(
        [
            "",
            "## Full matrix",
            "",
            "| Policy | Profile | Scenario | Presented % | Incomplete % | p50 delay | p95 delay | p99 delay | Max delay | Repair p95 | Missing | Recovered | Late | NACK | RTX loss | Bad IDR |",
            "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for row in rows:
        lines.append(
            "| {policy} | {profile} | {scenario} | {presented_pct} | {incomplete_pct} | {p50_frame_delay_ms} | {p95_frame_delay_ms} | {p99_frame_delay_ms} | {max_frame_delay_ms} | {p95_repair_wait_ms} | {missing_packets} | {recovered_packets} | {late_packets} | {nack_packets} | {retransmit_losses} | {corrupt_keyframes} |".format(**row)
        )
    lines.extend(
        [
            "",
            "## Caveats",
            "",
            "- This is a deterministic policy model, not a Switch hardware measurement.",
            "- The target policy is proposed network behavior and is not yet wired into production C++.",
            "- Actual H.264 reference-chain behavior, SRTP scheduling, socket-buffer overflow, and server-side retransmission must be validated with mock WebRTC and real hardware.",
            "",
        ]
    )
    path.write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("/tmp/lunarnx-xbox-video-policy-bench"),
    )
    args = parser.parse_args()

    all_runs: list[RunStats] = []
    matrix = scenarios()
    for scenario in matrix:
        for policy in policies(scenario.profile):
            for seed in range(1, scenario.seeds + 1):
                all_runs.append(run_once(scenario, policy, seed))
    rows = aggregate(all_runs)
    write_csv(args.output_dir / "xbox_rtp_network_benchmark.csv", rows)
    write_markdown(args.output_dir / "xbox_rtp_network_report.md", rows)
    print(f"runs={len(all_runs)} scenarios={len(matrix)}")
    print(f"report={args.output_dir / 'xbox_rtp_network_report.md'}")
    print(f"csv={args.output_dir / 'xbox_rtp_network_benchmark.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
