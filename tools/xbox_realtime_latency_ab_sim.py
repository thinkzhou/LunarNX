#!/usr/bin/env python3
from __future__ import annotations

"""Deterministic old/new Xbox realtime-pipeline comparison.

This is a queueing model, not an input-to-photon claim. Parameters include the
latest hardware trace's 60 fps source, roughly 56 fps presentation cadence and
9.5 ms mean RTP frame packet span. It reports user-facing freshness, display
gaps, intentional stale-frame drops, input owner-pump latency, decode-stall
recovery, audio queue depth and underruns.
"""

from collections import deque
from dataclasses import dataclass
import math
import random
import statistics


SOURCE_FPS = 60.0
FRAME_MS = 1000.0 / SOURCE_FPS
VIDEO_SECONDS = 60
VIDEO_FRAMES = int(SOURCE_FPS * VIDEO_SECONDS)


@dataclass(frozen=True)
class VideoScenario:
    name: str
    assembly_mean_ms: float
    assembly_jitter_ms: float
    display_fps: float = 56.0
    periodic_delay_ms: float = 0.0
    ui_stall_ms: float = 0.0
    degraded_start_ms: float = math.inf
    degraded_end_ms: float = -math.inf


@dataclass
class VideoResult:
    assembly_ms: list[float]
    queue_ms: list[float]
    glass_ms: list[float]
    gaps_ms: list[float]
    presented: int
    stale_drops: int
    capacity_drops: int


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = int(round((len(ordered) - 1) * quantile))
    return ordered[index]


def mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def run_video(seed: int, scenario: VideoScenario, new_policy: bool) -> VideoResult:
    rng = random.Random(seed)
    completions: list[tuple[float, float, float]] = []
    assembly_values: list[float] = []
    for frame in range(VIDEO_FRAMES):
        pts = frame * FRAME_MS
        assembly = max(
            0.15,
            rng.gauss(scenario.assembly_mean_ms,
                      scenario.assembly_jitter_ms))
        # A delayed packet episode every five seconds. The complete AU becomes
        # late; the renderer then decides whether stale decoded work is useful.
        if scenario.periodic_delay_ms and frame > 0 and frame % 300 == 0:
            assembly += scenario.periodic_delay_ms
        one_way_ms = 6.5 + rng.uniform(-0.5, 0.5)
        decode_handoff_ms = max(0.05, rng.gauss(0.35, 0.12))
        complete = pts + one_way_ms + assembly + decode_handoff_ms
        completions.append((complete, pts, assembly))
        assembly_values.append(assembly)
    completions.sort()

    pending: deque[tuple[float, float]] = deque()
    completion_index = 0
    queue_values: list[float] = []
    glass_values: list[float] = []
    gaps: list[float] = []
    presented = 0
    stale_drops = 0
    capacity_drops = 0
    last_present: float | None = None
    display_period = 1000.0 / scenario.display_fps
    tick = 0.0
    end = VIDEO_SECONDS * 1000.0 + 500.0
    while tick <= end:
        while (completion_index < len(completions) and
               completions[completion_index][0] <= tick):
            complete, pts, _ = completions[completion_index]
            completion_index += 1
            if len(pending) == 2:
                pending.popleft()
                capacity_drops += 1
            pending.append((complete, pts))

        in_ui_stall = (scenario.ui_stall_ms > 0.0 and
                       30_000.0 <= tick < 30_000.0 + scenario.ui_stall_ms)
        if not in_ui_stall and pending:
            degraded = (scenario.degraded_start_ms <= tick <
                        scenario.degraded_end_ms)
            adaptive = new_policy and not degraded
            oldest_wait = tick - pending[0][0]
            if adaptive and len(pending) > 1 and oldest_wait >= 25.0:
                stale_drops += len(pending) - 1
                while len(pending) > 1:
                    pending.popleft()
            complete, pts = pending.popleft()
            queue_values.append(tick - complete)
            glass_values.append(tick - pts)
            if last_present is not None:
                gaps.append(tick - last_present)
            last_present = tick
            presented += 1
        tick += display_period

    return VideoResult(
        assembly_values, queue_values, glass_values, gaps, presented,
        stale_drops, capacity_drops)


def aggregate_video(scenario: VideoScenario, new_policy: bool) -> dict[str, float]:
    results = [run_video(seed, scenario, new_policy) for seed in range(20)]
    assembly = [v for result in results for v in result.assembly_ms]
    queue = [v for result in results for v in result.queue_ms]
    glass = [v for result in results for v in result.glass_ms]
    gaps = [v for result in results for v in result.gaps_ms]
    return {
        "assembly_mean": mean(assembly),
        "assembly_p95": percentile(assembly, 0.95),
        "queue_mean": mean(queue),
        "queue_p95": percentile(queue, 0.95),
        "glass_p95": percentile(glass, 0.95),
        "gap_p95": percentile(gaps, 0.95),
        "gap_max": max(gaps) if gaps else 0.0,
        "fps": mean([r.presented / VIDEO_SECONDS for r in results]),
        "drops_per_s": mean([
            (r.stale_drops + r.capacity_drops) / VIDEO_SECONDS
            for r in results
        ]),
    }


@dataclass(frozen=True)
class AudioPolicy:
    name: str
    buffer_ms: int
    buffers: int
    max_writer_wait_ms: int

    @property
    def capacity_ms(self) -> int:
        return self.buffer_ms * self.buffers


@dataclass(frozen=True)
class AudioScenario:
    name: str
    recovery_gap_ms: int = 0


@dataclass
class AudioResult:
    queue_ms: list[float]
    underrun_ms: int
    dropped_ms: int


OLD_AUDIO = AudioPolicy("old", 25, 5, 25)
NEW_BALANCED_AUDIO = AudioPolicy("new", 25, 4, 25)
NEW_RECOVERY_AUDIO = AudioPolicy("new-recovery", 25, 5, 25)


@dataclass
class InputResult:
    sample_to_wire_ms: list[float]
    sent: int


def run_input(seed: int, predrain: bool) -> InputResult:
    """Model the 8 ms producer and 2 ms libpeer owner loop.

    The distribution matches the measured shape from the hardware trace: a
    roughly 1.5-2 ms peer loop with occasional 10-20 ms receive/RTP tails.
    Both policies send the same latest-state snapshots; only their position
    relative to inbound work changes.
    """
    rng = random.Random(seed)
    duration_ms = 60_000.0
    sample_period_ms = 8.0
    pump_period_ms = 2.0
    pump_phase_ms = rng.uniform(0.0, pump_period_ms)
    latencies: list[float] = []
    sample = 0.0
    while sample < duration_ms:
        pump_index = max(0, math.ceil(
            (sample - pump_phase_ms) / pump_period_ms))
        pump_start = pump_phase_ms + pump_index * pump_period_ms
        peer_loop_ms = max(0.15, rng.gauss(1.65, 0.55))
        if rng.random() < 0.004:
            peer_loop_ms += rng.uniform(8.0, 20.0)
        sctp_send_ms = max(0.03, rng.gauss(0.12, 0.03))
        wire = pump_start + sctp_send_ms
        if not predrain:
            wire += peer_loop_ms
        latencies.append(wire - sample)
        sample += sample_period_ms
    return InputResult(latencies, len(latencies))


def aggregate_input(predrain: bool) -> dict[str, float]:
    results = [run_input(seed, predrain) for seed in range(20)]
    samples = [value for result in results
               for value in result.sample_to_wire_ms]
    return {
        "mean": mean(samples),
        "p95": percentile(samples, 0.95),
        "p99": percentile(samples, 0.99),
        "max": max(samples),
        "sent_per_s": mean([result.sent / VIDEO_SECONDS
                            for result in results]),
    }


@dataclass(frozen=True)
class DecodeCatchUpScenario:
    name: str
    mode: str
    queued_frames: int
    oldest_age_ms: float


@dataclass(frozen=True)
class DecodeCatchUpResult:
    decoded: int
    renderer_handoffs: int
    suppressed: int
    capacity_drops: int
    first_present_age_ms: float
    latest_present_delay_ms: float


def catchup_decision(mode: str, active: bool, queued_behind: int,
                     queue_age_ms: float) -> tuple[bool, bool]:
    backlog_threshold = 2 if mode == "home" else 4
    stale_backlog_threshold = 1 if mode == "home" else 2
    stale_age_ms = 20.0 if mode == "home" else 60.0
    active = (active or queued_behind >= backlog_threshold or
              (queued_behind >= stale_backlog_threshold and
               queue_age_ms >= stale_age_ms))
    suppress = active and queued_behind > 0
    if active and queued_behind == 0:
        active = False
    return active, suppress


def run_decode_catchup(scenario: DecodeCatchUpScenario,
                       enabled: bool) -> DecodeCatchUpResult:
    # H.264 dependency frames are always decoded. A two-slot renderer keeps
    # only the newest handoffs if the decoder produces a burst after a stall.
    renderer: deque[int] = deque()
    active = False
    suppressed = 0
    capacity_drops = 0
    for frame in range(scenario.queued_frames):
        behind = scenario.queued_frames - frame - 1
        age_ms = max(0.0, scenario.oldest_age_ms - frame * 0.35)
        suppress = False
        if enabled:
            active, suppress = catchup_decision(
                scenario.mode, active, behind, age_ms)
        if suppress:
            suppressed += 1
            continue
        if len(renderer) == 2:
            renderer.popleft()
            capacity_drops += 1
        renderer.append(frame)

    first = renderer[0]
    latest = scenario.queued_frames - 1
    first_age_ms = (latest - first) * FRAME_MS
    latest_delay_ms = (len(renderer) - 1) * FRAME_MS
    return DecodeCatchUpResult(
        decoded=scenario.queued_frames,
        renderer_handoffs=scenario.queued_frames - suppressed,
        suppressed=suppressed,
        capacity_drops=capacity_drops,
        first_present_age_ms=first_age_ms,
        latest_present_delay_ms=latest_delay_ms)


def audio_release_times(duration_ms: int, recovery_gap_ms: int) -> list[int]:
    packet_ms = 20
    packet_count = duration_ms // packet_ms
    release: list[int] = []
    recovery_until = -1
    for packet in range(packet_count):
        nominal = max(0, (packet - 4) * packet_ms)
        if packet == 500 and recovery_gap_ms:
            recovery_until = nominal + recovery_gap_ms
        if recovery_until >= 0:
            nominal = max(nominal, recovery_until)
            if packet * packet_ms >= recovery_until + 80:
                recovery_until = -1
        release.append(nominal)
    return release


def run_audio(policy: AudioPolicy,
              scenario: AudioScenario,
              recovery_policy: AudioPolicy | None = None) -> AudioResult:
    duration_ms = 30_000
    releases = audio_release_times(duration_ms, scenario.recovery_gap_ms)
    arrivals: deque[int] = deque(releases)
    waiting: deque[int] = deque()
    queued = 0
    queue_values: list[float] = []
    underrun = 0
    dropped = 0
    started = False
    recovery_capacity_active = False
    for now in range(duration_ms):
        active_policy = policy
        if recovery_policy is not None and scenario.recovery_gap_ms:
            # The path/recovery watchdog enters resilient mode as soon as the
            # packet gap is observed, then keeps it for five clean windows.
            recovery_start = 9_900
            recovery_end = 10_000 + scenario.recovery_gap_ms + 5_000
            if recovery_start <= now < recovery_end:
                recovery_capacity_active = True
            elif (recovery_capacity_active and
                  (queued > policy.capacity_ms or waiting)):
                # A live player cannot hide a still-queued fifth wave buffer.
                # Let it drain before reducing the active ring from five to
                # four; otherwise the policy change itself drops audio.
                recovery_capacity_active = True
            else:
                recovery_capacity_active = False
            if recovery_capacity_active:
                active_policy = recovery_policy
        while arrivals and arrivals[0] <= now:
            waiting.append(arrivals.popleft())
        while waiting and queued + 20 <= active_policy.capacity_ms:
            waiting.popleft()
            queued += 20
            started = True
        while (waiting and
               now - waiting[0] > active_policy.max_writer_wait_ms):
            waiting.popleft()
            dropped += 20
        if started:
            if queued > 0:
                queued -= 1
            else:
                underrun += 1
        if now >= 1000:
            queue_values.append(float(queued))
    return AudioResult(queue_values, underrun, dropped)


def main() -> None:
    scenarios = [
        VideoScenario("clean_lan", 3.0, 0.8),
        VideoScenario("recorded_home", 9.5, 2.0),
        VideoScenario("packet_delay_50", 9.5, 2.0,
                      periodic_delay_ms=50.0),
        VideoScenario("ui_stall_50", 9.5, 2.0, ui_stall_ms=50.0),
        VideoScenario("sustained_fair", 12.0, 3.0,
                      periodic_delay_ms=30.0,
                      degraded_start_ms=20_000.0,
                      degraded_end_ms=35_000.0),
    ]
    print("Video A/B (20 seeds, 60 fps source, 56 fps display)")
    print("scenario          policy asmAvg asmP95 queueAvg queueP95 glassP95 "
          "fps drops/s gapP95 gapMax")
    measurements: dict[tuple[str, str], dict[str, float]] = {}
    for scenario in scenarios:
        for label, new_policy in (("old", False), ("new", True)):
            value = aggregate_video(scenario, new_policy)
            measurements[(scenario.name, label)] = value
            print(
                f"{scenario.name:<17} {label:<6} "
                f"{value['assembly_mean']:6.1f} {value['assembly_p95']:6.1f} "
                f"{value['queue_mean']:8.1f} {value['queue_p95']:8.1f} "
                f"{value['glass_p95']:8.1f} {value['fps']:5.1f} "
                f"{value['drops_per_s']:7.2f} {value['gap_p95']:6.1f} "
                f"{value['gap_max']:6.1f}")

    print("\nInput A/B (8 ms state producer, 2 ms owner pump)")
    print("policy sampleToWireAvg p95 p99 max sent/s")
    input_measurements: dict[str, dict[str, float]] = {}
    for label, predrain in (("old", False), ("new", True)):
        value = aggregate_input(predrain)
        input_measurements[label] = value
        print(
            f"{label:<6} {value['mean']:15.2f} {value['p95']:4.2f} "
            f"{value['p99']:4.2f} {value['max']:5.2f} "
            f"{value['sent_per_s']:6.1f}")

    catchup_scenarios = [
        DecodeCatchUpScenario("home_jitter_25", "home", 2, 25.0),
        DecodeCatchUpScenario("home_stall_50", "home", 4, 50.0),
        DecodeCatchUpScenario("cloud_jitter_50", "cloud", 4, 50.0),
        DecodeCatchUpScenario("cloud_stall_80", "cloud", 5, 80.0),
    ]
    print("\nDecode-stall A/B (all H.264 dependencies still decoded)")
    print("scenario          policy decoded handoffs suppressed capDrop "
          "firstAge latestDelay")
    catchup_measurements: dict[
        tuple[str, str], DecodeCatchUpResult] = {}
    for scenario in catchup_scenarios:
        for label, enabled in (("old", False), ("new", True)):
            value = run_decode_catchup(scenario, enabled)
            catchup_measurements[(scenario.name, label)] = value
            print(
                f"{scenario.name:<17} {label:<6} {value.decoded:7d} "
                f"{value.renderer_handoffs:8d} {value.suppressed:10d} "
                f"{value.capacity_drops:7d} "
                f"{value.first_present_age_ms:8.1f} "
                f"{value.latest_present_delay_ms:11.1f}")

    audio_scenarios = [
        AudioScenario("clean"),
        AudioScenario("recovery_gap_50", 50),
        AudioScenario("recovery_gap_90", 90),
        AudioScenario("recovery_gap_150", 150),
    ]
    print("\nAudio A/B (20 ms Opus, five-packet startup burst)")
    print("scenario          policy capacity queueAvg queueP95 underrun dropped")
    audio_measurements: dict[tuple[str, str], AudioResult] = {}
    for scenario in audio_scenarios:
        for policy, recovery_policy in (
                (OLD_AUDIO, None),
                (NEW_BALANCED_AUDIO, NEW_RECOVERY_AUDIO)):
            result = run_audio(policy, scenario, recovery_policy)
            audio_measurements[(scenario.name, policy.name)] = result
            capacity = (f"{policy.capacity_ms}->{recovery_policy.capacity_ms}"
                        if recovery_policy is not None and
                        scenario.recovery_gap_ms else
                        str(policy.capacity_ms))
            print(
                f"{scenario.name:<17} {policy.name:<6} "
                f"{capacity:>8} {mean(result.queue_ms):8.1f} "
                f"{percentile(result.queue_ms, 0.95):8.1f} "
                f"{result.underrun_ms:8d} {result.dropped_ms:7d}")

    recorded_old = measurements[("recorded_home", "old")]
    recorded_new = measurements[("recorded_home", "new")]
    assert 9.0 <= recorded_old["assembly_mean"] <= 10.0
    assert recorded_new["assembly_mean"] == recorded_old["assembly_mean"]
    assert recorded_new["queue_mean"] + 5.0 < recorded_old["queue_mean"]
    assert recorded_new["fps"] >= recorded_old["fps"] - 0.1
    assert recorded_new["gap_p95"] <= recorded_old["gap_p95"] + 0.1
    assert input_measurements["new"]["mean"] + 1.0 < \
        input_measurements["old"]["mean"]
    assert input_measurements["new"]["p99"] + 2.0 < \
        input_measurements["old"]["p99"]
    assert input_measurements["new"]["max"] + 10.0 < \
        input_measurements["old"]["max"]
    assert input_measurements["new"]["sent_per_s"] == \
        input_measurements["old"]["sent_per_s"]
    for scenario_name in ("home_jitter_25", "home_stall_50",
                          "cloud_stall_80"):
        old = catchup_measurements[(scenario_name, "old")]
        new = catchup_measurements[(scenario_name, "new")]
        assert new.decoded == old.decoded
        assert new.first_present_age_ms < old.first_present_age_ms
        assert new.latest_present_delay_ms < old.latest_present_delay_ms
    cloud_jitter_old = catchup_measurements[("cloud_jitter_50", "old")]
    cloud_jitter_new = catchup_measurements[("cloud_jitter_50", "new")]
    assert cloud_jitter_new == cloud_jitter_old
    for scenario_name in ("recovery_gap_50", "recovery_gap_90",
                          "recovery_gap_150"):
        assert (audio_measurements[(scenario_name, "new")].underrun_ms <=
                audio_measurements[(scenario_name, "old")].underrun_ms)
        assert (audio_measurements[(scenario_name, "new")].dropped_ms <=
                audio_measurements[(scenario_name, "old")].dropped_ms)
    assert NEW_BALANCED_AUDIO.capacity_ms < OLD_AUDIO.capacity_ms
    print("\nAll realtime latency A/B assertions passed")


if __name__ == "__main__":
    main()
