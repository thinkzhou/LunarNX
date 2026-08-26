#!/usr/bin/env python3
"""Deterministic old/current Xbox video-stall strategy simulation.

This is a scheduling/recovery model, not a Deko3D emulator.  It replays the
timings observed in a real LunarNX log and compares the old unbounded command
ring + shared decoder watchdog against the current bounded ring + split
renderer/decode/RTP watchdog.
"""

from __future__ import annotations

import argparse
import math
import random
import re
import statistics
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Optional


FRAME_MS = 1000.0 / 60.0
HEALTH_POLL_MS = 50
PRESENT_STALL_MS = 1000
DECODE_STALL_MS = 2000
RTP_STALL_MS = 3000
RECOVERY_COOLDOWN_MS = 2000
NEW_FENCE_WAIT_MS = 2.0
# 45 x 60 Hz ~= 750 ms.  This leaves the renderer's bounded 250 ms handoff
# one full chance to finish before the 1 s presentation watchdog fires, while
# allowing ordinary 50-500 ms GPU scheduling blips to recover by retry alone.
NEW_FENCE_TIMEOUT_THRESHOLD = 45
RENDERER_HANDOFF_TIMEOUT_MS = 250
RENDERER_DRAIN_PASSES = 4
RENDER_QUEUE_FRAMES = 3


@dataclass(frozen=True)
class Interval:
    start_ms: int
    end_ms: Optional[int]

    def contains(self, now_ms: float) -> bool:
        return now_ms >= self.start_ms and (
            self.end_ms is None or now_ms < self.end_ms
        )

    def remaining(self, now_ms: float) -> float:
        if self.end_ms is None:
            return math.inf
        return max(0.0, self.end_ms - now_ms)


@dataclass(frozen=True)
class Scenario:
    name: str
    duration_ms: int
    fence_stalls: tuple[Interval, ...] = ()
    decode_stalls: tuple[Interval, ...] = ()
    rtp_stalls: tuple[Interval, ...] = ()


@dataclass
class Result:
    scenario: str
    policy: str
    ui_max_block_ms: float = 0.0
    freeze_max_ms: float = 0.0
    skipped_present_frames: int = 0
    render_queue_drops: int = 0
    decode_paused_frames: int = 0
    decoder_resets: int = 0
    renderer_recoveries: int = 0
    renderer_recovery_failures: int = 0
    reconnects: int = 0
    stream_errors: int = 0
    app_hung: bool = False
    session_owner_hung: bool = False
    active_until_ms: int = 0
    events: list[str] = field(default_factory=list)

    @property
    def outcome(self) -> str:
        if self.app_hung or self.session_owner_hung:
            return "hung"
        if self.stream_errors:
            return "stream-error"
        if self.reconnects:
            return "reconnected"
        return "continued"


def active_interval(intervals: Iterable[Interval], now_ms: float) -> Optional[Interval]:
    return next((interval for interval in intervals if interval.contains(now_ms)), None)


def percentile(values: list[float], percentile_value: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1,
                max(0, math.ceil(percentile_value * len(ordered)) - 1))
    return ordered[index]


def simulate(scenario: Scenario, policy: str) -> Result:
    result = Result(scenario=scenario.name, policy=policy)
    now_ms = 0
    next_frame_ms = 0.0
    next_health_ms = 0
    last_rtp_ms = 0.0
    last_decode_ms = 0.0
    last_present_ms = 0.0
    last_present_for_freeze_ms = 0.0
    pending_frames = 0

    old_ui_blocked_until = 0.0
    old_health_attempts = 0
    renderer_attempts = 0
    decoder_attempts = 0
    last_health_recovery_ms: Optional[int] = None

    consecutive_fence_timeouts = 0
    consecutive_render_faults = 0
    renderer_pending = False
    renderer_reset_requested = False
    renderer_reset_deadline_ms = 0.0
    renderer_drain_passes = 0
    video_worker_paused_until = 0.0
    stream_active = True

    def record_present(at_ms: float) -> None:
        nonlocal last_present_ms, last_present_for_freeze_ms, pending_frames
        nonlocal consecutive_render_faults, renderer_pending
        result.freeze_max_ms = max(
            result.freeze_max_ms, at_ms - last_present_for_freeze_ms
        )
        last_present_for_freeze_ms = at_ms
        last_present_ms = at_ms
        consecutive_render_faults = 0
        if pending_frames:
            pending_frames -= 1
        # A timed-out GPU handoff may recover just after the worker's 250 ms
        # deadline.  A proven successful present must retire stale pending state.
        if policy == "current" and renderer_pending and not renderer_reset_requested:
            renderer_pending = False
            result.events.append(f"{at_ms:.0f}:late-renderer-recovery")

    def start_renderer_recovery(at_ms: float, source: str) -> None:
        nonlocal renderer_pending, renderer_reset_requested
        nonlocal renderer_reset_deadline_ms, renderer_drain_passes
        nonlocal video_worker_paused_until
        if renderer_pending:
            return
        renderer_pending = True
        renderer_reset_requested = True
        renderer_reset_deadline_ms = at_ms + RENDERER_HANDOFF_TIMEOUT_MS
        renderer_drain_passes = 0
        video_worker_paused_until = renderer_reset_deadline_ms
        result.renderer_recoveries += 1
        result.events.append(f"{at_ms:.0f}:renderer-recovery:{source}")

    while now_ms <= scenario.duration_ms and stream_active:
        fence_stall = active_interval(scenario.fence_stalls, now_ms)
        decode_stall = active_interval(scenario.decode_stalls, now_ms)
        rtp_stall = active_interval(scenario.rtp_stalls, now_ms)

        if rtp_stall is None:
            last_rtp_ms = now_ms

        while next_frame_ms <= now_ms + 1e-6:
            frame_time = next_frame_ms
            next_frame_ms += FRAME_MS

            worker_paused = frame_time < video_worker_paused_until
            if rtp_stall is None and decode_stall is None and not worker_paused:
                last_decode_ms = frame_time
                if pending_frames >= RENDER_QUEUE_FRAMES:
                    result.render_queue_drops += 1
                else:
                    pending_frames += 1
            elif worker_paused:
                result.decode_paused_frames += 1

            if policy == "old":
                if result.app_hung:
                    result.skipped_present_frames += 1
                    continue
                if frame_time < old_ui_blocked_until:
                    result.skipped_present_frames += 1
                    continue
                frame_fence_stall = active_interval(scenario.fence_stalls, frame_time)
                if frame_fence_stall is not None:
                    remaining = frame_fence_stall.remaining(frame_time)
                    result.skipped_present_frames += 1
                    if math.isinf(remaining):
                        result.ui_max_block_ms = math.inf
                        result.app_hung = True
                        result.events.append(f"{frame_time:.0f}:ui-fence-wait:infinite")
                    else:
                        result.ui_max_block_ms = max(result.ui_max_block_ms,
                                                     remaining)
                        old_ui_blocked_until = frame_time + remaining
                        result.events.append(
                            f"{frame_time:.0f}:ui-fence-wait:{remaining:.0f}ms"
                        )
                    continue
                record_present(frame_time)
                continue

            # Current implementation: each command-ring wait is bounded.  A
            # renderer reset drains one no-op list per Borealis frame.
            frame_fence_stall = active_interval(scenario.fence_stalls, frame_time)
            fence_remaining = (
                frame_fence_stall.remaining(frame_time)
                if frame_fence_stall is not None else 0.0
            )
            fence_ready = fence_remaining <= NEW_FENCE_WAIT_MS
            wait_ms = min(fence_remaining, NEW_FENCE_WAIT_MS)
            result.ui_max_block_ms = max(result.ui_max_block_ms, wait_ms)

            if renderer_reset_requested:
                if not fence_ready:
                    result.skipped_present_frames += 1
                    continue
                renderer_drain_passes += 1
                if renderer_drain_passes >= RENDERER_DRAIN_PASSES:
                    renderer_reset_requested = False
                    renderer_pending = False
                    video_worker_paused_until = frame_time + wait_ms
                    result.events.append(f"{frame_time + wait_ms:.0f}:renderer-resumed")
                continue

            if not fence_ready:
                result.skipped_present_frames += 1
                consecutive_fence_timeouts += 1
                if consecutive_fence_timeouts >= NEW_FENCE_TIMEOUT_THRESHOLD:
                    consecutive_fence_timeouts = 0
                    consecutive_render_faults += 1
                    start_renderer_recovery(frame_time + wait_ms,
                                            "command-fence-timeout")
                continue

            consecutive_fence_timeouts = 0
            record_present(frame_time + wait_ms)

        if (policy == "current" and renderer_reset_requested and
                now_ms >= renderer_reset_deadline_ms):
            renderer_reset_requested = False
            video_worker_paused_until = now_ms
            result.renderer_recovery_failures += 1
            result.events.append(f"{now_ms}:renderer-handoff-timeout")

        if now_ms >= next_health_ms:
            next_health_ms += HEALTH_POLL_MS
            rtp_age = now_ms - last_rtp_ms
            decode_age = now_ms - last_decode_ms
            present_age = now_ms - last_present_ms
            rtp_stalled = rtp_age >= RTP_STALL_MS
            rtp_alive = rtp_age < RTP_STALL_MS
            decode_stalled = rtp_alive and decode_age >= DECODE_STALL_MS
            present_stalled = (
                last_decode_ms > 0 and
                (present_age >= PRESENT_STALL_MS or
                 consecutive_render_faults >= 2)
            )
            recovery_due = (
                last_health_recovery_ms is None or
                now_ms - last_health_recovery_ms >= RECOVERY_COOLDOWN_MS
            )

            if policy == "old":
                if rtp_stalled:
                    result.reconnects += 1
                    result.events.append(f"{now_ms}:rtp-reconnect")
                    stream_active = False
                elif recovery_due and (decode_stalled or present_stalled):
                    if old_health_attempts >= 1:
                        if result.app_hung:
                            result.session_owner_hung = True
                            result.events.append(
                                f"{now_ms}:reconnect-blocked-by-ui-lifecycle"
                            )
                        else:
                            result.reconnects += 1
                            result.events.append(f"{now_ms}:pipeline-reconnect")
                        stream_active = False
                    else:
                        old_health_attempts += 1
                        last_health_recovery_ms = now_ms
                        result.decoder_resets += 1
                        video_worker_paused_until = max(
                            video_worker_paused_until, now_ms + 250
                        )
                        result.events.append(f"{now_ms}:decoder-recovery")
                elif not decode_stalled and not present_stalled and present_age < 1000:
                    old_health_attempts = 0
            else:
                if rtp_stalled:
                    result.reconnects += 1
                    result.events.append(f"{now_ms}:rtp-reconnect")
                    stream_active = False
                elif recovery_due and decode_stalled:
                    if decoder_attempts:
                        result.reconnects += 1
                        result.events.append(f"{now_ms}:decode-reconnect")
                        stream_active = False
                    else:
                        decoder_attempts += 1
                        last_health_recovery_ms = now_ms
                        result.decoder_resets += 1
                        video_worker_paused_until = max(
                            video_worker_paused_until, now_ms + 250
                        )
                        result.events.append(f"{now_ms}:decoder-recovery")
                elif recovery_due and present_stalled:
                    if renderer_attempts:
                        result.stream_errors += 1
                        result.events.append(f"{now_ms}:renderer-stream-error")
                        stream_active = False
                    elif renderer_pending:
                        renderer_attempts += 1
                        last_health_recovery_ms = now_ms
                        result.events.append(f"{now_ms}:renderer-recovery-grace")
                    else:
                        renderer_attempts += 1
                        last_health_recovery_ms = now_ms
                        start_renderer_recovery(now_ms, "present-watchdog")
                elif (not decode_stalled and not present_stalled and
                      present_age < 1000 and consecutive_render_faults == 0):
                    renderer_attempts = 0
                    decoder_attempts = 0
                    last_health_recovery_ms = None

        now_ms += 1

    result.active_until_ms = now_ms
    if last_present_for_freeze_ms:
        freeze_end = min(now_ms, scenario.duration_ms)
        result.freeze_max_ms = max(
            result.freeze_max_ms, freeze_end - last_present_for_freeze_ms
        )
    return result


def extract_log_calibration(log_path: Path) -> tuple[list[tuple[int, int]], list[int]]:
    text = log_path.read_text(errors="replace")
    present_stalls = [
        (int(decode_age), int(present_age))
        for decode_age, present_age in re.findall(
            r"pipeline stall decode_age_ms=(\d+) present_age_ms=(\d+).*"
            r"action=decoder-recovery",
            text,
        )
        if int(decode_age) < 100 and int(present_age) >= 1000
    ]
    persistent_ages = [
        int(present_age)
        for present_age in re.findall(
            r"persistent pipeline stall decode_age_ms=\d+ present_age_ms=(\d+)",
            text,
        )
        if int(present_age) >= 1000
    ]
    return present_stalls, persistent_ages


def fixed_scenarios() -> list[Scenario]:
    return [
        Scenario("log_present_permanent", 6500,
                 fence_stalls=(Interval(1000, None),)),
        Scenario("fence_blip_50", 3000,
                 fence_stalls=(Interval(1000, 1050),)),
        Scenario("fence_blip_100", 3000,
                 fence_stalls=(Interval(1000, 1100),)),
        Scenario("fence_blip_180", 3500,
                 fence_stalls=(Interval(1000, 1180),)),
        Scenario("fence_blip_350", 4000,
                 fence_stalls=(Interval(1000, 1350),)),
        Scenario("fence_blip_500", 4500,
                 fence_stalls=(Interval(1000, 1500),)),
        Scenario("repeated_late_1200", 8000,
                 fence_stalls=(Interval(1000, 2200), Interval(4500, 5700))),
        Scenario("decode_stall_2500", 6000,
                 decode_stalls=(Interval(1000, 3500),)),
        Scenario("rtp_stall_3500", 6000,
                 rtp_stalls=(Interval(1000, 4500),)),
    ]


def random_scenario(seed: int) -> Scenario:
    rng = random.Random(seed)
    intervals: list[Interval] = []
    cursor = 800
    for _ in range(rng.randint(2, 6)):
        cursor += rng.randint(250, 900)
        if rng.random() < 0.04:
            intervals.append(Interval(cursor, None))
            break
        duration = rng.choice([8, 16, 33, 50, 100, 180, 350, 500, 1200])
        intervals.append(Interval(cursor, cursor + duration))
        cursor += duration
    return Scenario(f"mixed_{seed:03d}", 9000,
                    fence_stalls=tuple(intervals))


def format_ms(value: float) -> str:
    return "INF" if math.isinf(value) else f"{value:.0f}"


def print_fixed(results: list[Result]) -> None:
    print("scenario               policy   uiMax freeze skip qDrop decR renR renFail reconn outcome")
    for result in results:
        print(
            f"{result.scenario:<22} {result.policy:<8} "
            f"{format_ms(result.ui_max_block_ms):>5} "
            f"{result.freeze_max_ms:>6.0f} "
            f"{result.skipped_present_frames:>4} "
            f"{result.render_queue_drops:>5} "
            f"{result.decoder_resets:>4} "
            f"{result.renderer_recoveries:>4} "
            f"{result.renderer_recovery_failures:>7} "
            f"{result.reconnects:>6} {result.outcome}"
        )


def print_random_summary(results: list[Result], seed_count: int) -> None:
    print(f"\n{seed_count}-seed mixed fence-stall stress")
    print("policy uiBlockP95 uiBlockWorst freezeP95 freezeWorst queueDropAvg "
          "decReset hang streamError")
    for policy in ("old", "current"):
        subset = [result for result in results if result.policy == policy]
        finite_blocks = [
            result.ui_max_block_ms
            for result in subset
            if not math.isinf(result.ui_max_block_ms)
        ]
        worst_block = max((result.ui_max_block_ms for result in subset), default=0)
        print(
            f"{policy:<7} "
            f"{format_ms(percentile(finite_blocks, 0.95)):>10} "
            f"{format_ms(worst_block):>12} "
            f"{percentile([r.freeze_max_ms for r in subset], 0.95):>9.0f} "
            f"{max(r.freeze_max_ms for r in subset):>11.0f} "
            f"{statistics.mean(r.render_queue_drops for r in subset):>12.1f} "
            f"{sum(r.decoder_resets for r in subset):>8} "
            f"{sum(r.outcome == 'hung' for r in subset):>4} "
            f"{sum(r.stream_errors for r in subset):>11}"
        )


def assert_expected(fixed: dict[tuple[str, str], Result],
                    random_results: list[Result]) -> None:
    old_log = fixed[("log_present_permanent", "old")]
    new_log = fixed[("log_present_permanent", "current")]
    assert old_log.outcome == "hung"
    assert new_log.outcome == "stream-error"
    assert new_log.ui_max_block_ms <= NEW_FENCE_WAIT_MS
    assert new_log.decoder_resets == 0 and new_log.reconnects == 0

    for name in ("fence_blip_50", "fence_blip_100", "fence_blip_180",
                 "fence_blip_350", "fence_blip_500"):
        current = fixed[(name, "current")]
        assert current.outcome == "continued"
        assert current.decoder_resets == 0 and current.reconnects == 0
        assert current.renderer_recoveries == 0
        assert current.ui_max_block_ms <= NEW_FENCE_WAIT_MS

    repeated = fixed[("repeated_late_1200", "current")]
    assert repeated.outcome == "continued"
    assert repeated.renderer_recoveries >= 2, (
        "late successful presentation must clear stale renderer recovery state"
    )

    for name in ("decode_stall_2500", "rtp_stall_3500"):
        old = fixed[(name, "old")]
        current = fixed[(name, "current")]
        assert old.decoder_resets == current.decoder_resets
        assert old.reconnects == current.reconnects

    old_hangs = sum(result.outcome == "hung" for result in random_results
                    if result.policy == "old")
    new_hangs = sum(result.outcome == "hung" for result in random_results
                    if result.policy == "current")
    assert new_hangs == 0 and old_hangs > 0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--log",
        default="/Users/zhouyang/Downloads/lunarnx[1] (1).log",
        type=Path,
    )
    parser.add_argument("--seeds", type=int, default=100)
    args = parser.parse_args()
    if args.seeds <= 0:
        raise SystemExit("--seeds must be positive")

    present_stalls, persistent_ages = extract_log_calibration(args.log)
    if not present_stalls or not persistent_ages:
        raise SystemExit("log does not contain the expected present-stall signature")
    print("Log calibration")
    print(f"present-only stalls={present_stalls}")
    print(f"persistent present ages={persistent_ages}")
    print(
        "observed first/persistent thresholds ~= "
        f"{statistics.mean(age for _, age in present_stalls):.0f}/"
        f"{statistics.mean(persistent_ages):.0f} ms\n"
    )

    fixed_results = [
        simulate(scenario, policy)
        for scenario in fixed_scenarios()
        for policy in ("old", "current")
    ]
    print_fixed(fixed_results)

    random_results = [
        simulate(random_scenario(seed), policy)
        for seed in range(args.seeds)
        for policy in ("old", "current")
    ]
    print_random_summary(random_results, args.seeds)

    fixed_by_key = {
        (result.scenario, result.policy): result
        for result in fixed_results
    }
    assert_expected(fixed_by_key, random_results)
    print("\nAll video recovery A/B simulation checks passed")


if __name__ == "__main__":
    main()
