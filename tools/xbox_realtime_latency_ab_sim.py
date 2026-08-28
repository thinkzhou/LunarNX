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
    ingress_packets: int = 512
    startup_packets: int = 1

    @property
    def capacity_ms(self) -> int:
        return self.buffer_ms * self.buffers


@dataclass(frozen=True)
class AudioScenario:
    name: str
    recovery_gap_ms: int = 0
    periodic_gap_ms: int = 0
    periodic_interval_ms: int = 0


@dataclass
class AudioResult:
    hardware_queue_ms: list[float]
    total_backlog_ms: list[float]
    underrun_ms: int
    dropped_ms: int
    startup_skipped_ms: int
    live_edge_resets: int


@dataclass(frozen=True)
class AudioStrategy:
    name: str
    scope: str
    realtime: AudioPolicy
    balanced: AudioPolicy
    recovery: AudioPolicy
    startup_gate: bool = False
    drain_downshift: bool = False
    preserve_hardware_on_catchup: bool = False


@dataclass(frozen=True)
class AvSyncScenario:
    name: str
    callback_skew_ms: float
    queued_audio_ms: float
    corrected_arrival_skew_ms: float
    new_audio_ms: float
    base_video_ms: float = 15.0


@dataclass(frozen=True)
class AvSyncResult:
    clock_error_ms: float
    audible_skew_ms: float
    video_latency_ms: float
    audio_latency_ms: float
    presentation_hold_ms: float


def run_av_sync(scenario: AvSyncScenario, fixed: bool) -> AvSyncResult:
    if not fixed:
        audio_latency = scenario.queued_audio_ms
        return AvSyncResult(
            scenario.callback_skew_ms,
            abs(audio_latency - scenario.base_video_ms),
            scenario.base_video_ms,
            audio_latency,
            0.0)

    # The fixed mapper subtracts libpeer's recorded queue age from each first
    # callback. Startup audio is gated until video exists, so only the bounded
    # hardware queue remains. Do not delay the two-slot video handoff to erase
    # the final few milliseconds of skew: that can starve presentation.
    clock_error = scenario.corrected_arrival_skew_ms
    audio_latency = scenario.new_audio_ms
    hold = 0.0
    video_latency = scenario.base_video_ms
    return AvSyncResult(
        clock_error,
        abs(audio_latency - video_latency),
        video_latency,
        audio_latency,
        hold)


OLD_HOME_AUDIO = AudioPolicy("home-old", 20, 4, 20)
OLD_CLOUD_AUDIO = AudioPolicy("cloud-old", 25, 4, 25)
OLD_CLOUD_RECOVERY_AUDIO = AudioPolicy("cloud-old-recovery", 25, 5, 25)
NEW_REALTIME_AUDIO = AudioPolicy(
    "new-realtime", 20, 3, 20, ingress_packets=6, startup_packets=3)
NEW_BALANCED_AUDIO = AudioPolicy(
    "new-balanced", 20, 4, 20, ingress_packets=7, startup_packets=3)
NEW_RECOVERY_AUDIO = AudioPolicy(
    "new-recovery", 20, 5, 20, ingress_packets=7, startup_packets=4)

HOME_OLD = AudioStrategy(
    "home-old", "home", OLD_HOME_AUDIO, OLD_HOME_AUDIO, OLD_HOME_AUDIO)
HOME_NEW = AudioStrategy(
    "home-new", "home", NEW_REALTIME_AUDIO,
    NEW_BALANCED_AUDIO, NEW_BALANCED_AUDIO, True, True, True)
CLOUD_OLD = AudioStrategy(
    "cloud-old", "cloud", OLD_CLOUD_AUDIO,
    OLD_CLOUD_AUDIO, OLD_CLOUD_RECOVERY_AUDIO)
CLOUD_NEW = AudioStrategy(
    "cloud-new", "cloud", NEW_REALTIME_AUDIO,
    NEW_BALANCED_AUDIO, NEW_RECOVERY_AUDIO, True, True, True)


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


def audio_release_times(duration_ms: int,
                        scenario: AudioScenario) -> list[int]:
    packet_ms = 20
    packet_count = duration_ms // packet_ms
    release: list[int] = []
    recovery_until = -1
    for packet in range(packet_count):
        nominal = max(0, (packet - 4) * packet_ms)
        if packet == 500 and scenario.recovery_gap_ms:
            recovery_until = nominal + scenario.recovery_gap_ms
        if recovery_until >= 0:
            nominal = max(nominal, recovery_until)
            if packet * packet_ms >= recovery_until + 80:
                recovery_until = -1
        if (scenario.periodic_gap_ms and
                scenario.periodic_interval_ms > 0):
            episode = nominal // scenario.periodic_interval_ms
            if episode > 0:
                episode_start = episode * scenario.periodic_interval_ms
                if nominal < episode_start + scenario.periodic_gap_ms:
                    nominal = episode_start + scenario.periodic_gap_ms
        release.append(nominal)
    return release


def requested_audio_policy(strategy: AudioStrategy,
                           scenario: AudioScenario,
                           now: int) -> AudioPolicy:
    if strategy.scope == "home":
        if not scenario.recovery_gap_ms:
            return strategy.realtime
        # Home switches to Balanced on the poor/recovery observation, then
        # returns to Realtime on the first clean one-second path window.
        recovery_end = 10_000 + scenario.recovery_gap_ms + 1_000
        return (strategy.balanced
                if 9_900 <= now < recovery_end else strategy.realtime)

    # Cloud starts Balanced. Eight clean estimator windows promote it to
    # Realtime; Recovery needs five clean windows to fall back to Balanced and
    # another eight to reach Realtime again.
    if not scenario.recovery_gap_ms:
        return strategy.balanced if now < 8_000 else strategy.realtime
    recovery_end = 10_000 + scenario.recovery_gap_ms + 5_000
    if 9_900 <= now < recovery_end:
        return strategy.recovery
    if now < 8_000 or now < recovery_end + 8_000:
        return strategy.balanced
    return strategy.realtime


def run_audio(strategy: AudioStrategy,
              scenario: AudioScenario) -> AudioResult:
    duration_ms = 30_000
    releases = audio_release_times(duration_ms, scenario)
    arrivals: deque[int] = deque(releases)
    waiting: deque[int] = deque()
    queued = 0
    hardware_values: list[float] = []
    backlog_values: list[float] = []
    underrun = 0
    dropped = 0
    startup_skipped = 0
    live_edge_resets = 0
    started = False
    ever_started = False
    primed = not strategy.startup_gate
    writer_wait_ms = 0
    active_policy = requested_audio_policy(strategy, scenario, 0)
    for now in range(duration_ms):
        requested_policy = requested_audio_policy(strategy, scenario, now)
        while arrivals and arrivals[0] <= now:
            release = arrivals.popleft()
            if strategy.startup_gate and release == 0:
                startup_skipped += 20
                continue
            waiting.append(release)
            if len(waiting) > requested_policy.ingress_packets:
                newest = waiting[-1]
                dropped += (len(waiting) - 1) * 20
                waiting.clear()
                waiting.append(newest)
                if not strategy.preserve_hardware_on_catchup:
                    queued = 0
                    started = False
                primed = strategy.preserve_hardware_on_catchup
                writer_wait_ms = 0
                live_edge_resets += 1
        if (not primed and
                len(waiting) >= requested_policy.startup_packets):
            primed = True
        if started and now > 0:
            if queued > 0:
                queued -= 1
        if (ever_started and queued == 0 and
                1_000 <= now < duration_ms - 200):
            underrun += 1
        if requested_policy.capacity_ms >= active_policy.capacity_ms:
            active_policy = requested_policy
        elif queued <= requested_policy.capacity_ms:
            # writeAudio asks Audren for fresh descriptor state before trying
            # the downshift, so apply this after the millisecond of playback.
            active_policy = requested_policy
        # Model the serial audio worker, including writeAudio's bounded Audren
        # wait. A requested downshift stops recycling descriptors outside the
        # smaller ring, allowing the old ring to drain instead of remaining
        # permanently full. Realtime then gives it 5 ms before shedding stale
        # work; Balanced/Recovery tolerate one 20 ms Opus period.
        blocked = False
        while primed and waiting:
            write_capacity_ms = active_policy.capacity_ms
            if strategy.drain_downshift:
                write_capacity_ms = min(write_capacity_ms,
                                        requested_policy.capacity_ms)
            if queued + 20 <= write_capacity_ms:
                waiting.popleft()
                queued += 20
                writer_wait_ms = 0
                started = True
                ever_started = True
                continue
            wait_policy = (requested_policy if strategy.drain_downshift
                           else active_policy)
            if writer_wait_ms >= wait_policy.max_writer_wait_ms:
                waiting.popleft()
                dropped += 20
                writer_wait_ms = 0
                continue
            blocked = True
            break
        if blocked:
            writer_wait_ms += 1
        elif not waiting:
            writer_wait_ms = 0
        # Exclude startup prebuffering and the synthetic source's finite tail.
        if 1_000 <= now < duration_ms - 200:
            hardware_values.append(float(queued))
            backlog_values.append(float(queued + len(waiting) * 20))
    return AudioResult(hardware_values, backlog_values, underrun, dropped,
                       startup_skipped, live_edge_resets)


@dataclass(frozen=True)
class PresentationGateResult:
    presented_fps: float
    maximum_gap_ms: float
    dropped_per_second: float


def run_presentation_gate(delay_ms: float,
                          rejected_gate: bool,
                          duration_ms: int = 10_000) -> PresentationGateResult:
    """Model render() handoff immediately before present() at 60 Hz.

    The rejected implementation attached an A/V-clock release deadline to each
    frame in the two-slot renderer queue. When a new frame arrived in the same
    UI iteration that the head became ready, enqueue replaced that head before
    present could consume it. The fixed policy keeps A/V timing out of this
    bounded latest-frame handoff.
    """
    frame_period_ms = 1000.0 / SOURCE_FPS
    pending: deque[tuple[float, float]] = deque()
    next_frame_ms = 0.0
    tick_ms = 0.0
    presented_at: list[float] = []
    dropped = 0
    while tick_ms < duration_ms:
        while next_frame_ms <= tick_ms + 1e-9:
            release_ms = (next_frame_ms + delay_ms
                          if rejected_gate else 0.0)
            item = (next_frame_ms, release_ms)
            if len(pending) == 2:
                if rejected_gate and tick_ms < pending[0][1]:
                    pending[-1] = item
                else:
                    pending.popleft()
                    pending.append(item)
                dropped += 1
            else:
                pending.append(item)
            next_frame_ms += frame_period_ms

        head_ready = (pending and
                      (not rejected_gate or
                       tick_ms + 1e-9 >= pending[0][1]))
        if head_ready:
            while (len(pending) > 1 and
                   (not rejected_gate or
                    tick_ms + 1e-9 >= pending[1][1])):
                pending.popleft()
                dropped += 1
            pending.popleft()
            presented_at.append(tick_ms)
        tick_ms += frame_period_ms

    gaps = [new - old for old, new in
            zip(presented_at, presented_at[1:])]
    seconds = duration_ms / 1000.0
    return PresentationGateResult(
        presented_fps=len(presented_at) / seconds,
        maximum_gap_ms=max(gaps) if gaps else duration_ms,
        dropped_per_second=dropped / seconds)


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
        AudioScenario("jitter_30_every_2s", periodic_gap_ms=30,
                      periodic_interval_ms=2_000),
        AudioScenario("jitter_50_every_5s", periodic_gap_ms=50,
                      periodic_interval_ms=5_000),
        AudioScenario("recovery_gap_50", 50),
        AudioScenario("recovery_gap_90", 90),
        AudioScenario("recovery_gap_150", 150),
        AudioScenario("recovery_gap_250", 250),
    ]
    print("\nAudio A/B (20 ms Opus, five-packet startup burst)")
    print("scenario          policy      hwAvg  hwP95 totalAvg totalP95 "
          "steadyP95 underrun dropped startupSkip catchup")
    audio_measurements: dict[tuple[str, str], AudioResult] = {}
    audio_strategies = (HOME_OLD, HOME_NEW, CLOUD_OLD, CLOUD_NEW)
    for scenario in audio_scenarios:
        for strategy in audio_strategies:
            result = run_audio(strategy, scenario)
            audio_measurements[(scenario.name, strategy.name)] = result
            print(
                f"{scenario.name:<17} {strategy.name:<9} "
                f"{mean(result.hardware_queue_ms):6.1f} "
                f"{percentile(result.hardware_queue_ms, 0.95):6.1f} "
                f"{mean(result.total_backlog_ms):8.1f} "
                f"{percentile(result.total_backlog_ms, 0.95):8.1f} "
                f"{percentile(result.total_backlog_ms[-6_000:], 0.95):9.1f} "
                f"{result.underrun_ms:8d} {result.dropped_ms:7d} "
                f"{result.startup_skipped_ms:11d} "
                f"{result.live_edge_resets:7d}")

    av_scenarios = [
        AvSyncScenario("clean_lan", 5.0, 80.0, 5.0, 60.0),
        AvSyncScenario("startup_300", 300.0, 380.0, 7.0, 60.0),
        AvSyncScenario("recorded_900", 900.0, 980.0, 8.0, 60.0),
        AvSyncScenario("recovery_150", 150.0, 250.0, 15.0, 100.0),
    ]
    print("\nA/V startup/recovery A/B (queue-age anchors + live-edge audio)")
    print("scenario          policy clockErr avSkew videoLatency audioLatency hold")
    av_measurements: dict[tuple[str, str], AvSyncResult] = {}
    for scenario in av_scenarios:
        for label, fixed in (("old", False), ("new", True)):
            result = run_av_sync(scenario, fixed)
            av_measurements[(scenario.name, label)] = result
            print(
                f"{scenario.name:<17} {label:<6} "
                f"{result.clock_error_ms:8.1f} "
                f"{result.audible_skew_ms:6.1f} "
                f"{result.video_latency_ms:12.1f} "
                f"{result.audio_latency_ms:12.1f} "
                f"{result.presentation_hold_ms:5.1f}")

    print("\nRenderer A/V-gate regression (60 fps render-before-present)")
    print("delayMs policy   fps maxGap drops/s")
    presentation_measurements: dict[
        tuple[float, str], PresentationGateResult] = {}
    for delay_ms in (0.0, 20.0, 40.0, 80.0, 200.0):
        for label, rejected_gate in (("rejected", True), ("fixed", False)):
            result = run_presentation_gate(delay_ms, rejected_gate)
            presentation_measurements[(delay_ms, label)] = result
            print(f"{delay_ms:7.1f} {label:<8} "
                  f"{result.presented_fps:5.1f} "
                  f"{result.maximum_gap_ms:6.1f} "
                  f"{result.dropped_per_second:7.1f}")

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
    for scope in ("home", "cloud"):
        old_clean = audio_measurements[("clean", f"{scope}-old")]
        new_clean = audio_measurements[("clean", f"{scope}-new")]
        assert mean(new_clean.hardware_queue_ms) + 15.0 < \
            mean(old_clean.hardware_queue_ms)
        assert mean(new_clean.total_backlog_ms) + 15.0 < \
            mean(old_clean.total_backlog_ms)
        assert percentile(new_clean.total_backlog_ms[-6_000:], 0.95) + 15.0 < \
            percentile(old_clean.total_backlog_ms[-6_000:], 0.95)
        assert new_clean.underrun_ms == old_clean.underrun_ms == 0
        # Startup packets received before the first video frame are skipped,
        # so they never become audible latency after the picture appears.
        assert old_clean.dropped_ms == 0
        # Cloud intentionally sheds one 20 ms packet when its clean-window
        # policy contracts Balanced (60 ms) to Realtime (40 ms). Home starts
        # at Realtime and does not need that one-time timeline contraction.
        assert new_clean.dropped_ms <= (20 if scope == "cloud" else 0)
        assert new_clean.startup_skipped_ms == 100
        for scenario_name in ("recovery_gap_50", "recovery_gap_90",
                              "recovery_gap_150", "recovery_gap_250"):
            old = audio_measurements[(scenario_name, f"{scope}-old")]
            new = audio_measurements[(scenario_name, f"{scope}-new")]
            scenario_gap_ms = int(scenario_name.rsplit("_", 1)[1])
            # The 60 ms realtime ring keeps one extra Opus packet of scheduling
            # tolerance while remaining below the old 80/100 ms buffering.
            # Longer stalls may still shed stale recovered packets.
            assert new.underrun_ms <= old.underrun_ms + 80
            assert new.dropped_ms <= scenario_gap_ms + 40
            assert mean(new.total_backlog_ms) + 5.0 < \
                mean(old.total_backlog_ms)
            steady_limit = 120.0
            assert percentile(new.total_backlog_ms[-6_000:], 0.95) <= \
                steady_limit
        for scenario_name in ("jitter_30_every_2s", "jitter_50_every_5s"):
            old = audio_measurements[(scenario_name, f"{scope}-old")]
            new = audio_measurements[(scenario_name, f"{scope}-new")]
            assert new.underrun_ms <= old.underrun_ms + 20
            assert new.dropped_ms == old.dropped_ms
    assert NEW_REALTIME_AUDIO.capacity_ms < OLD_HOME_AUDIO.capacity_ms
    assert NEW_REALTIME_AUDIO.capacity_ms < OLD_CLOUD_AUDIO.capacity_ms
    recorded_av_old = av_measurements[("recorded_900", "old")]
    recorded_av_new = av_measurements[("recorded_900", "new")]
    assert recorded_av_old.clock_error_ms >= 800.0
    assert recorded_av_new.clock_error_ms <= 10.0
    assert recorded_av_old.audible_skew_ms >= 800.0
    assert recorded_av_new.audible_skew_ms <= 50.0
    for scenario in av_scenarios:
        new = av_measurements[(scenario.name, "new")]
        assert new.audible_skew_ms <= 90.0
        assert new.video_latency_ms == scenario.base_video_ms
        assert new.presentation_hold_ms == 0.0
    assert presentation_measurements[(20.0, "rejected")].presented_fps < 10.0
    for delay_ms in (0.0, 20.0, 40.0, 80.0, 200.0):
        fixed = presentation_measurements[(delay_ms, "fixed")]
        assert fixed.presented_fps >= 59.9
        assert fixed.maximum_gap_ms <= FRAME_MS + 0.1
    print("\nAll realtime latency A/B assertions passed")


if __name__ == "__main__":
    main()
