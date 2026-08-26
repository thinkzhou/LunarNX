#!/usr/bin/env python3
"""Deterministic Xbox gamepad delivery strategy A/B/C simulation."""

from __future__ import annotations

from dataclasses import dataclass
import random
import statistics


DURATION_MS = 30_000
SAMPLE_MS = 8
SNAPSHOT_MS = 16
OLD_HEARTBEAT_MS = 250
PUMP_MS = 2
WIRE_OVERHEAD_BYTES = 80

OLD = "old"
COMPLEX = "complex16"
GREEN = "green8"
HYBRID = "hybrid50"
TRANSITION_TTL_MS = 50


@dataclass
class Transition:
    ident: int
    state: bool
    occurred_ms: int


@dataclass
class Packet:
    frames: list[bool]
    transitions: list[Transition]
    last_transition_id: int
    latest_generation: int
    includes_latest: bool
    sequence: int = -1

    @property
    def revision(self) -> tuple[int, int, bool]:
        return (self.last_transition_id,
                self.latest_generation,
                self.includes_latest)

    @property
    def payload_bytes(self) -> int:
        return 15 + 23 * len(self.frames)


@dataclass
class Metrics:
    packets: float
    dropped: float
    wire_kbps: float
    mismatch_pct: float
    sticky_p95_ms: float
    sticky_max_ms: float
    release_p95_ms: float
    release_max_ms: float
    press_p95_ms: float
    missed_taps_pct: float


class Producer:
    def __init__(self, strategy: str):
        self.strategy = strategy
        self.sampled_state = False
        self.has_sample = False
        self.snapshot_state = False
        self.has_snapshot = False
        self.latest_state = False
        self.latest_dirty = False
        self.latest_generation = 0
        self.latest_transition: Transition | None = None
        self.transitions: list[Transition] = []
        self.next_transition_id = 1
        self.next_snapshot_ms = 0
        self.next_heartbeat_ms = 0
        self.pending: Packet | None = None

    def sample(self, now_ms: int, physical_state: bool) -> None:
        transition = self.has_sample and physical_state != self.sampled_state
        event = None
        if transition:
            event = Transition(self.next_transition_id,
                               physical_state, now_ms)
            self.next_transition_id += 1

        self.latest_state = physical_state
        self.sampled_state = physical_state
        self.has_sample = True

        if self.strategy in (GREEN, HYBRID):
            # Green-NX: one full current state per 8 ms sample. There is no
            # retry queue; a newer generation replaces an unsent one. LunarNX
            # additionally journals digital edges for at most 50 ms.
            self.latest_dirty = True
            self.latest_generation += 1
            self.latest_transition = event
            if self.strategy == HYBRID and event:
                self.transitions.append(event)
            return

        if event:
            self.transitions.append(event)
        snapshot_due = now_ms >= self.next_snapshot_ms
        heartbeat_due = now_ms >= self.next_heartbeat_ms
        snapshot_changed = (not self.has_snapshot or
                            physical_state != self.snapshot_state)
        mark_latest = snapshot_due and (
            self.strategy == COMPLEX or snapshot_changed)
        force_latest = heartbeat_due and self.strategy == OLD
        if transition or mark_latest or force_latest:
            self.latest_dirty = True
            self.latest_generation += 1
        if snapshot_due:
            self.snapshot_state = physical_state
            self.has_snapshot = True
            self.next_snapshot_ms = now_ms + SNAPSHOT_MS
        if heartbeat_due:
            self.next_heartbeat_ms = now_ms + OLD_HEARTBEAT_MS

    def packet(self, now_ms: int) -> Packet | None:
        if self.strategy == HYBRID:
            while (self.transitions and
                   now_ms - self.transitions[0].occurred_ms >
                   TRANSITION_TTL_MS):
                self.transitions.pop(0)
            if self.transitions:
                transition = self.transitions[0]
                return Packet(
                    frames=[transition.state],
                    transitions=[transition],
                    last_transition_id=transition.ident,
                    latest_generation=0,
                    includes_latest=False,
                )

        if self.strategy in (GREEN, HYBRID):
            if not self.latest_dirty:
                return None
            transitions = ([self.latest_transition]
                           if self.latest_transition else [])
            return Packet(
                frames=[self.latest_state],
                transitions=transitions,
                last_transition_id=0,
                latest_generation=self.latest_generation,
                includes_latest=True,
            )

        if not self.transitions and not self.latest_dirty:
            return None
        transitions = self.transitions[:29]
        frames = [item.state for item in transitions]
        includes_latest = len(transitions) == len(self.transitions)
        if includes_latest and self.latest_dirty:
            if not frames or frames[-1] != self.latest_state:
                frames.append(self.latest_state)
        return Packet(
            frames=frames,
            transitions=transitions,
            last_transition_id=(transitions[-1].ident
                                if transitions else 0),
            latest_generation=(self.latest_generation
                               if includes_latest else 0),
            includes_latest=includes_latest,
        )

    def refresh_pending(self, now_ms: int) -> None:
        if (self.strategy == HYBRID and self.pending is not None and
                self.pending.transitions and
                now_ms - self.pending.transitions[0].occurred_ms >
                TRANSITION_TTL_MS):
            self.pending = None
        candidate = self.packet(now_ms)
        if candidate is None:
            return
        if self.pending is None:
            self.pending = candidate
        elif self.strategy == HYBRID:
            pending_transition = bool(self.pending.transitions)
            candidate_transition = bool(candidate.transitions)
            if (not pending_transition and candidate_transition) or (
                    not pending_transition and
                    candidate.revision != self.pending.revision):
                self.pending = candidate
        elif self.strategy != OLD and candidate.revision != self.pending.revision:
            self.pending = candidate

    def commit_pending(self) -> Packet:
        assert self.pending is not None
        sent = self.pending
        if self.strategy != GREEN and sent.last_transition_id:
            self.transitions = [
                item for item in self.transitions
                if item.ident > sent.last_transition_id
            ]
        if (sent.includes_latest and
                sent.latest_generation == self.latest_generation):
            self.latest_dirty = False
        self.pending = None
        return sent


def percentile(values: list[int], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1,
                int((len(ordered) - 1) * fraction))
    return float(ordered[index])


def tap_events() -> tuple[dict[int, bool], list[tuple[int, int]]]:
    events: dict[int, bool] = {}
    taps: list[tuple[int, int]] = []
    # Deliberately offset events from the 8 ms sampler to include polling
    # latency instead of giving every strategy a zero-cost sample.
    start = 257
    index = 0
    while start < DURATION_MS - 700:
        hold = (48, 56, 80, 88)[index % 4]
        release = start + hold
        events[start] = True
        events[release] = False
        taps.append((start, release))
        start = release + (384, 432, 480)[index % 3]
        index += 1
    return events, taps


def backpressure_windows(taps: list[tuple[int, int]],
                         duration_ms: int) -> list[tuple[int, int]]:
    return [
        (press - 2, press - 2 + duration_ms)
        for press, _ in taps[4::11]
    ]


def transition_overlap_windows(
        taps: list[tuple[int, int]]) -> list[tuple[int, int]]:
    # Block after the press has been sampled, then unblock just after the
    # shortest release was sampled. Latest-only delivery sees only "released";
    # the bounded edge journal can still deliver press then current release.
    return [
        (press + 6, press + 55)
        for press, _ in taps[4::11]
    ]


def in_windows(now_ms: int,
               windows: list[tuple[int, int]]) -> bool:
    return any(start <= now_ms < end for start, end in windows)


def extra_delay(now_ms: int,
                windows: list[tuple[int, int, int]]) -> int:
    return max((delay for start, end, delay in windows
                if start <= now_ms < end), default=0)


def simulate(strategy: str,
             seed: int,
             one_way_ms: int = 8,
             random_loss: float = 0.0,
             drop_release_transitions: bool = False,
             drop_windows: list[tuple[int, int]] | None = None,
             blocked_windows: list[tuple[int, int]] | None = None,
             delay_windows: list[tuple[int, int, int]] | None = None) -> Metrics:
    drop_windows = drop_windows or []
    blocked_windows = blocked_windows or []
    delay_windows = delay_windows or []
    rng = random.Random(seed)
    events, taps = tap_events()
    producer = Producer(strategy)
    physical_state = False
    server_state = False
    scheduled: dict[int, list[Packet]] = {}
    delivered_frames: list[tuple[int, bool]] = []
    timeline_len = DURATION_MS + one_way_ms + 1200
    server_timeline = [False] * timeline_len
    packets = 0
    dropped = 0
    wire_bytes = 0
    mismatch_ms = 0
    sticky_runs: list[int] = []
    sticky_run = 0
    dropped_release_ids: set[int] = set()
    next_sequence = 0
    last_server_sequence = -1

    for now_ms in range(timeline_len):
        if now_ms in scheduled:
            for packet in scheduled[now_ms]:
                # XStreaming's sequence field lets the receiver reject a
                # delayed older packet on the unordered channel.
                if packet.sequence <= last_server_sequence:
                    continue
                last_server_sequence = packet.sequence
                for state in packet.frames:
                    server_state = state
                    delivered_frames.append((now_ms, state))

        if now_ms < DURATION_MS:
            if now_ms in events:
                physical_state = events[now_ms]
            if now_ms % SAMPLE_MS == 0:
                producer.sample(now_ms, physical_state)
            if now_ms % PUMP_MS == 0:
                producer.refresh_pending(now_ms)
                if (producer.pending is not None and
                        not in_windows(now_ms, blocked_windows)):
                    packet = producer.commit_pending()
                    packet.sequence = next_sequence
                    next_sequence += 1
                    packets += 1
                    wire_bytes += packet.payload_bytes + WIRE_OVERHEAD_BYTES
                    release_ids = [
                        item.ident for item in packet.transitions
                        if not item.state
                    ]
                    targeted_drop = False
                    if drop_release_transitions:
                        for ident in release_ids:
                            if ident not in dropped_release_ids:
                                dropped_release_ids.add(ident)
                                targeted_drop = True
                                break
                    packet_dropped = (
                        targeted_drop or
                        in_windows(now_ms, drop_windows) or
                        rng.random() < random_loss
                    )
                    if packet_dropped:
                        dropped += 1
                    else:
                        delay = one_way_ms + extra_delay(now_ms, delay_windows)
                        scheduled.setdefault(now_ms + delay, []).append(packet)

        server_timeline[now_ms] = server_state
        if now_ms < DURATION_MS and server_state != physical_state:
            mismatch_ms += 1
        if now_ms < DURATION_MS and not physical_state and server_state:
            sticky_run += 1
        elif sticky_run:
            sticky_runs.append(sticky_run)
            sticky_run = 0
    if sticky_run:
        sticky_runs.append(sticky_run)

    press_latencies: list[int] = []
    release_latencies: list[int] = []
    missed = 0
    for press, release in taps:
        press_delivery = next(
            (time for time, state in delivered_frames
             if state and press <= time < release + 300),
            None,
        )
        if press_delivery is None:
            missed += 1
        else:
            press_latencies.append(press_delivery - press)

        if not server_timeline[release]:
            release_latencies.append(0)
        else:
            repaired = next(
                (time for time in range(
                    release, min(len(server_timeline), release + 1000))
                 if not server_timeline[time]),
                release + 1000,
            )
            release_latencies.append(repaired - release)

    return Metrics(
        packets=float(packets),
        dropped=float(dropped),
        wire_kbps=wire_bytes * 8.0 / DURATION_MS,
        mismatch_pct=mismatch_ms * 100.0 / DURATION_MS,
        sticky_p95_ms=percentile(sticky_runs, 0.95),
        sticky_max_ms=float(max(sticky_runs, default=0)),
        release_p95_ms=percentile(release_latencies, 0.95),
        release_max_ms=float(max(release_latencies, default=0)),
        press_p95_ms=percentile(press_latencies, 0.95),
        missed_taps_pct=missed * 100.0 / len(taps),
    )


def average_metrics(samples: list[Metrics]) -> Metrics:
    return Metrics(**{
        field: statistics.mean(getattr(sample, field) for sample in samples)
        for field in Metrics.__dataclass_fields__
    })


def main() -> None:
    _, taps = tap_events()
    loss_bursts = [
        (taps[index][1] - 2, taps[index][1] + 78)
        for index in range(3, len(taps), 13)
    ]
    delay_spikes = [
        (taps[index][0] - 10, taps[index][0] + 70, 50)
        for index in range(2, len(taps), 13)
    ]
    scenarios = [
        ("local_clean_2ms", dict(one_way_ms=2)),
        ("lan_clean_8ms", dict(one_way_ms=8)),
        ("cloud_clean_35ms", dict(one_way_ms=35)),
        ("lan_loss_0.5pct", dict(one_way_ms=8, random_loss=0.005)),
        ("lan_loss_2pct", dict(one_way_ms=8, random_loss=0.02)),
        ("release_packet_loss", dict(
            one_way_ms=8, drop_release_transitions=True)),
        ("uplink_loss_burst80", dict(
            one_way_ms=8, drop_windows=loss_bursts)),
        ("latency_spike_50", dict(
            one_way_ms=2, delay_windows=delay_spikes)),
        ("backpressure_50", dict(
            one_way_ms=8,
            blocked_windows=backpressure_windows(taps, 50))),
        ("transition_overlap49", dict(
            one_way_ms=8,
            blocked_windows=transition_overlap_windows(taps))),
        ("backpressure_100", dict(
            one_way_ms=8,
            blocked_windows=backpressure_windows(taps, 100))),
        ("backpressure_250", dict(
            one_way_ms=8,
            blocked_windows=backpressure_windows(taps, 250))),
    ]

    print("Xbox input delivery A/B/C/D simulation (40 deterministic seeds)")
    print("old=250ms idle repair + pending transition gate")
    print("complex16=16ms repair + replaceable transition batch")
    print("green8=8ms full current state + no transition queue/retry")
    print("hybrid50=green8 + ordered digital edges with a 50ms TTL")
    print("scenario             policy     pkt wireKb mismatch sticky95 "
          "stickyMax release95 releaseMax press95 missed%")
    results: dict[tuple[str, str], Metrics] = {}
    for name, options in scenarios:
        for strategy in (OLD, COMPLEX, GREEN, HYBRID):
            metrics = average_metrics([
                simulate(strategy, seed, **options)
                for seed in range(40)
            ])
            results[(name, strategy)] = metrics
            print(f"{name:20} {strategy:10} {metrics.packets:5.0f} "
                  f"{metrics.wire_kbps:6.1f} {metrics.mismatch_pct:8.3f} "
                  f"{metrics.sticky_p95_ms:8.1f} {metrics.sticky_max_ms:9.1f} "
                  f"{metrics.release_p95_ms:9.1f} "
                  f"{metrics.release_max_ms:10.1f} "
                  f"{metrics.press_p95_ms:7.1f} "
                  f"{metrics.missed_taps_pct:7.3f}")

    local_complex = results[("local_clean_2ms", COMPLEX)]
    local_green = results[("local_clean_2ms", GREEN)]
    release_old = results[("release_packet_loss", OLD)]
    release_complex = results[("release_packet_loss", COMPLEX)]
    release_green = results[("release_packet_loss", GREEN)]
    burst_complex = results[("uplink_loss_burst80", COMPLEX)]
    burst_green = results[("uplink_loss_burst80", GREEN)]
    blocked_complex = results[("backpressure_100", COMPLEX)]
    blocked_green = results[("backpressure_100", GREEN)]
    blocked_hybrid = results[("backpressure_100", HYBRID)]
    overlap_green = results[("transition_overlap49", GREEN)]
    overlap_hybrid = results[("transition_overlap49", HYBRID)]
    release_hybrid = results[("release_packet_loss", HYBRID)]

    assert local_green.press_p95_ms <= local_complex.press_p95_ms
    assert local_green.missed_taps_pct == 0
    assert release_green.sticky_p95_ms < release_complex.sticky_p95_ms
    assert release_complex.sticky_p95_ms * 3 < release_old.sticky_p95_ms
    assert release_green.release_p95_ms < release_complex.release_p95_ms
    assert burst_green.sticky_max_ms <= burst_complex.sticky_max_ms
    assert local_green.wire_kbps < 130
    assert results[("local_clean_2ms", HYBRID)].missed_taps_pct == 0
    assert results[("local_clean_2ms", HYBRID)].wire_kbps < 135
    assert overlap_hybrid.missed_taps_pct < overlap_green.missed_taps_pct
    assert overlap_hybrid.sticky_max_ms <= 20
    assert release_hybrid.sticky_p95_ms <= release_green.sticky_p95_ms
    # Explicitly preserve the policy tradeoff in the regression: a tap fully
    # contained in local send backpressure can be delivered late by the complex
    # transition queue, while the realtime policy intentionally drops it.
    assert blocked_green.missed_taps_pct >= blocked_complex.missed_taps_pct
    assert blocked_hybrid.missed_taps_pct <= blocked_green.missed_taps_pct
    print("Tradeoff: green8 prefers fresh state; a tap entirely inside local "
          "backpressure may be missed instead of replayed late.")
    print("All Xbox input strategy simulation checks passed")


if __name__ == "__main__":
    main()
