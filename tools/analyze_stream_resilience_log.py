#!/usr/bin/env python3
import argparse
import re
from pathlib import Path


FIELD = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")


def integer(value: str) -> int:
    match = re.match(r"-?[0-9]+", value)
    return int(match.group(0)) if match else 0


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Summarize LunarNX drop diagnostics for resilience comparisons")
    parser.add_argument("log", type=Path)
    args = parser.parse_args()

    rows = []
    for line in args.log.read_text(errors="replace").splitlines():
        if "[drop-diag " not in line:
            continue
        fields = dict(FIELD.findall(line))
        fields["line"] = str(len(rows) + 1)
        rows.append(fields)

    displayed = [integer(row["elapsed_ms"]) for row in rows
                 if row.get("phase") == "displayed"]
    pump_gaps = [integer(row.get("pump_gap_us", "0")) for row in rows]
    pump_durations = [integer(row.get("pump_duration_us", "0")) for row in rows]
    missing_deltas = [integer(row.get("missing_delta", "0")) for row in rows]
    loss_to_display = []
    for row in rows:
        if row.get("source") != "rtp_loss" or "t" not in row:
            continue
        loss_time = integer(row["t"])
        next_display = next(
            (integer(candidate["t"]) for candidate in rows
             if candidate.get("phase") == "displayed" and
             integer(candidate.get("t", "0")) >= loss_time),
            None)
        if next_display is not None:
            loss_to_display.append(next_display - loss_time)

    episode2 = [row for row in rows if row.get("episode") == "2"]
    episode2_nacks = [integer(row.get("nacks", "0")) for row in episode2]
    episode2_srtp = [integer(row.get("srtp_fail", "0")) for row in episode2
                     if "srtp_fail" in row]

    print(f"METRIC source=hardware_log recovery_samples={len(displayed)} "
          f"recovery_max_ms={max(displayed, default=0)}")
    print(f"METRIC source=hardware_log pump_gap_max_us={max(pump_gaps, default=0)} "
          f"pump_duration_max_us={max(pump_durations, default=0)}")
    print(f"METRIC source=hardware_log missing_delta_max={max(missing_deltas, default=0)}")
    print("METRIC source=hardware_log "
          f"loss_to_next_display_max_ms={max(loss_to_display, default=0)}")
    if episode2_nacks and episode2_srtp:
        print("METRIC source=hardware_log episode=2 "
              f"nack_growth={max(episode2_nacks) - min(episode2_nacks)} "
              f"srtp_failure_growth={max(episode2_srtp) - min(episode2_srtp)}")


if __name__ == "__main__":
    main()
