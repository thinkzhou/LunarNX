#!/usr/bin/env python3
"""Deterministic clean-path latency simulation for the video pipeline.

This is a timing model, not a hardware benchmark. It keeps the effects that
matter for the proposed A/Bs visible: ingress/copy and worker handoff,
serialized decode, a two-frame renderer queue, and presentation on a 60 Hz UI
tick. The network is intentionally clean (no loss, no IDR recovery); a small
arrival jitter and occasional 8 ms worker hiccup model ordinary scheduling
noise on a good connection.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
import random
from statistics import median


FPS = 60.0
FRAME_MS = 1000.0 / FPS
FRAME_COUNT = 60 * 30
PRESENT_QUEUE_LIMIT = 2
REALTIME_STALE_MS = 16.0


@dataclass
class DecodedFrame:
    sequence: int
    pts_ms: float
    decoded_ms: float
    decode_start_ms: float
    ingress_ready_ms: float


@dataclass
class SimulationResult:
    name: str
    latencies_ms: list[float]
    renderer_wait_ms: list[float]
    ingress_wait_ms: list[float]
    dropped_frames: int
    displayed_sequences: list[int]
    missed_display_ticks: int

    @property
    def p50(self) -> float:
        return median(self.latencies_ms)

    def percentile(self, value: float) -> float:
        ordered = sorted(self.latencies_ms)
        index = min(len(ordered) - 1, int((len(ordered) - 1) * value))
        return ordered[index]


def percentile(values: list[float], value: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, int((len(ordered) - 1) * value))
    return ordered[index]


def simulate(name: str,
             direct: bool,
             stale_ms: float | None,
             seed: int,
             latest_wins: bool = False,
             worker_stall_ms: float = 8.0,
             ui_stall_ms: float = 0.0,
             decoupled_present: bool = False) -> SimulationResult:
    rng = random.Random(seed)
    decoded: list[DecodedFrame] = []
    worker_free_ms = 0.0
    producer_free_ms = 0.0

    for sequence in range(FRAME_COUNT):
        pts_ms = sequence * FRAME_MS
        arrival_ms = pts_ms + rng.uniform(-0.45, 0.45)

        # A direct Xbox Home/PS callback avoids the AU copy plus a wakeup. It
        # still has a small callback/decoder handoff cost and remains
        # serialized.
        if direct:
            ingress_ready_ms = max(arrival_ms + 0.10, producer_free_ms)
            producer_free_ms = ingress_ready_ms
            copy_ms = 0.10
        else:
            ingress_ready_ms = max(arrival_ms + 0.35, producer_free_ms)
            producer_free_ms = ingress_ready_ms
            copy_ms = 0.35

        decode_start_ms = max(ingress_ready_ms, worker_free_ms)

        # Keep the hiccups sparse and bounded: this represents a good network
        # with local scheduler noise, not packet loss or recovery behavior.
        if sequence and sequence % 300 == 0:
            decode_start_ms = max(decode_start_ms, pts_ms + worker_stall_ms)

        decode_ms = 2.4 + rng.uniform(-0.25, 0.25)
        decoded_ms = decode_start_ms + decode_ms
        worker_free_ms = decoded_ms
        decoded.append(DecodedFrame(
            sequence,
            pts_ms,
            decoded_ms,
            decode_start_ms,
            ingress_ready_ms,
        ))

    pending: list[DecodedFrame] = []
    next_decoded = 0
    latencies: list[float] = []
    renderer_waits: list[float] = []
    ingress_waits: list[float] = []
    displayed: list[int] = []
    dropped = 0
    missed_display_ticks = 0
    ui_busy_until_ms = 0.0

    last_tick = (FRAME_COUNT + 2) * FRAME_MS
    tick = 0.0
    while tick <= last_tick:
        while next_decoded < len(decoded) and decoded[next_decoded].decoded_ms <= tick:
            pending.append(decoded[next_decoded])
            next_decoded += 1
            while len(pending) > PRESENT_QUEUE_LIMIT:
                pending.pop(0)
                dropped += 1

        if stale_ms is not None and len(pending) > 1:
            oldest_wait = tick - pending[0].decoded_ms
            if oldest_wait >= stale_ms:
                dropped += len(pending) - 1
                pending = pending[-1:]

        if latest_wins and len(pending) > 1:
            dropped += len(pending) - 1
            pending = pending[-1:]

        # The current path presents from the Borealis UI frame. A UI layout or
        # overlay stall can therefore miss this display interval entirely.
        # A truly independent presenter would keep consuming the display
        # cadence while UI work is busy; it still cannot exceed the 60 Hz
        # physical vblank cadence.
        ui_busy = tick < ui_busy_until_ms
        if ui_stall_ms > 0.0 and int(round(tick / FRAME_MS)) % 180 == 60:
            ui_busy_until_ms = max(ui_busy_until_ms, tick + ui_stall_ms)
            ui_busy = True

        if pending and (decoupled_present or not ui_busy):
            frame = pending.pop(0)
            displayed.append(frame.sequence)
            latencies.append(tick - frame.pts_ms)
            renderer_waits.append(tick - frame.decoded_ms)
            ingress_waits.append(frame.decode_start_ms - frame.ingress_ready_ms + copy_ms)
        elif pending and ui_busy:
            missed_display_ticks += 1

        tick += FRAME_MS

    return SimulationResult(
        name,
        latencies,
        renderer_waits,
        ingress_waits,
        dropped,
        displayed,
        missed_display_ticks,
    )


def assert_clean(result: SimulationResult) -> None:
    assert result.dropped_frames == 0, (result.name, result.dropped_frames)
    assert len(result.displayed_sequences) >= FRAME_COUNT - 1, result.name
    assert result.percentile(0.95) < 20.0, (result.name, result.percentile(0.95))


def simulate_renderer_lifetime_contention(seed: int = 7) -> tuple[list[float], list[float], int]:
    """A/B only the old lifecycle try-lock around a 60 Hz present tick.

    The decode callback is assumed to hold the old lifecycle mutex for a short
    renderer-handoff interval. The new path shares only renderer lifetime, so
    the decode callback and UI tick can proceed to their renderer-owned locks
    independently. This is intentionally a narrow contention model, not a
    claim about GPU throughput.
    """
    rng = random.Random(seed)
    old_latencies: list[float] = []
    new_latencies: list[float] = []
    old_missed = 0
    handoff_ms = 1.2
    for sequence in range(FRAME_COUNT):
        pts_ms = sequence * FRAME_MS
        callback_ms = pts_ms + rng.uniform(0.0, FRAME_MS)
        present_tick = math.ceil(callback_ms / FRAME_MS) * FRAME_MS
        old_present_tick = present_tick
        if callback_ms <= present_tick <= callback_ms + handoff_ms:
            old_present_tick += FRAME_MS
            old_missed += 1
        old_latencies.append(old_present_tick - pts_ms)
        new_latencies.append(present_tick - pts_ms)
    return old_latencies, new_latencies, old_missed


def main() -> None:
    results = [
        simulate("queued-xbox-cloud", direct=False, stale_ms=None, seed=7),
        simulate("queued-realtime-25ms", direct=False, stale_ms=25.0, seed=7),
        simulate("queued-realtime-16ms", direct=False, stale_ms=REALTIME_STALE_MS, seed=7),
        simulate("xbox-home-adaptive-16ms", direct=False,
                 stale_ms=REALTIME_STALE_MS, seed=7),
        simulate("xbox-home-direct-16ms", direct=True, stale_ms=REALTIME_STALE_MS, seed=7),
        simulate("ps-direct-adaptive-16ms", direct=True,
                 stale_ms=REALTIME_STALE_MS, seed=7),
    ]
    burst_fifo = simulate("burst-fifo-24ms", direct=False, stale_ms=None, seed=7,
                          worker_stall_ms=24.0)
    burst_adaptive = simulate("burst-adaptive-16ms", direct=False,
                              stale_ms=REALTIME_STALE_MS, seed=7,
                              worker_stall_ms=24.0)
    burst_latest = simulate("burst-latest-wins-24ms", direct=False, stale_ms=None,
                            seed=7, latest_wins=True, worker_stall_ms=24.0)

    ui_bound_fifo = simulate("ui-bound-fifo-18ms", direct=False,
                             stale_ms=None, seed=7,
                             ui_stall_ms=18.0)
    ui_bound = simulate("ui-bound-adaptive-18ms", direct=False,
                        stale_ms=REALTIME_STALE_MS, seed=7,
                        ui_stall_ms=18.0)
    ui_decoupled = simulate("independent-presenter-18ms", direct=False,
                            stale_ms=REALTIME_STALE_MS, seed=7,
                            ui_stall_ms=18.0, decoupled_present=True)
    old_lock_latencies, new_lock_latencies, old_lock_misses = \
        simulate_renderer_lifetime_contention()

    for result in results:
        assert_clean(result)

    queued = results[0]
    direct = results[4]
    assert percentile(direct.ingress_wait_ms, 0.95) < percentile(
        queued.ingress_wait_ms, 0.95
    ), (queued.ingress_wait_ms, direct.ingress_wait_ms)
    assert percentile(results[2].renderer_wait_ms, 0.95) <= percentile(
        results[1].renderer_wait_ms, 0.95
    )
    assert results[3].dropped_frames == results[5].dropped_frames
    assert burst_latest.dropped_frames > 0
    assert burst_latest.percentile(0.95) < burst_fifo.percentile(0.95)
    assert burst_adaptive.dropped_frames > 0
    assert burst_adaptive.percentile(0.95) < burst_fifo.percentile(0.95)
    assert ui_bound_fifo.percentile(0.99) - ui_decoupled.percentile(0.99) >= FRAME_MS
    assert ui_bound_fifo.missed_display_ticks > 0
    assert ui_bound.missed_display_ticks > ui_decoupled.missed_display_ticks
    assert ui_decoupled.dropped_frames < ui_bound.dropped_frames
    assert old_lock_misses > 0
    assert percentile(new_lock_latencies, 0.95) < percentile(
        old_lock_latencies, 0.95
    )

    print("mode                         p50    p95    p99   ingress-p95  render-p95  drops")
    for result in results:
        print(
            f"{result.name:28} {result.p50:5.2f}  "
            f"{result.percentile(0.95):5.2f}  {result.percentile(0.99):5.2f}  "
            f"{percentile(result.ingress_wait_ms, 0.95):11.2f}  "
            f"{percentile(result.renderer_wait_ms, 0.95):10.2f}  "
            f"{result.dropped_frames:5d}"
        )
    print("burst experiment (24 ms decoder stall):")
    for result in (burst_fifo, burst_adaptive, burst_latest):
        print(
            f"{result.name:28} {result.p50:5.2f}  "
            f"{result.percentile(0.95):5.2f}  {result.percentile(0.99):5.2f}  "
            f"{percentile(result.ingress_wait_ms, 0.95):11.2f}  "
            f"{percentile(result.renderer_wait_ms, 0.95):10.2f}  "
            f"{result.dropped_frames:5d}"
        )
    print("UI contention experiment (18 ms UI stall every 3 seconds):")
    for result in (ui_bound_fifo, ui_bound, ui_decoupled):
        print(
            f"{result.name:28} {result.p50:5.2f}  "
            f"{result.percentile(0.95):5.2f}  {result.percentile(0.99):5.2f}  "
            f"{percentile(result.ingress_wait_ms, 0.95):11.2f}  "
            f"{percentile(result.renderer_wait_ms, 0.95):10.2f}  "
            f"{result.dropped_frames:5d}  missed={result.missed_display_ticks}"
        )
    print(
        "UI contention FIFO p99 reduction: "
        f"{ui_bound_fifo.percentile(0.99) - ui_decoupled.percentile(0.99):.2f} ms"
    )
    print(
        "renderer lifetime A/B: "
        f"old p95={percentile(old_lock_latencies, 0.95):.2f} ms "
        f"new p95={percentile(new_lock_latencies, 0.95):.2f} ms "
        f"old missed ticks={old_lock_misses}"
    )
    print("clean-path assertions: PASS")


if __name__ == "__main__":
    main()
