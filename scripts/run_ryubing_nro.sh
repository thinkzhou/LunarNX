#!/bin/bash
# Run a Switch NRO with Ryubing Canary.
#
# Default behavior intentionally discards Ryubing stdout/stderr. Ryubing can
# spam non-fatal HLE warnings fast enough to create multi-GB logs. Use the
# app's own SD log for LunarNX diagnostics:
#   $RYUJINX_DATA/sdcard/switch/LunarNX/lunarnx.log
#
# Usage:
#   ./scripts/run_ryubing_nro.sh [path/to/name.nro]
#
# Optional:
#   RYUBING_LOG_MODE=filtered ./scripts/run_ryubing_nro.sh build/switch/LunarNX.nro
#   RYUBING_LOG_MODE=raw      ./scripts/run_ryubing_nro.sh build/switch/LunarNX.nro

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

RYUBING="${RYUBING:-/tmp/ryubing-canary-1.3.333/Ryujinx.app/Contents/MacOS/Ryujinx}"
RYUJINX_DATA="${RYUJINX_DATA:-$HOME/work/self/ryujinx-data}"
RYUBING_LOG_MODE="${RYUBING_LOG_MODE:-discard}"
NRO="${1:-$PROJECT_DIR/build/switch/LunarNX.nro}"

if [ ! -x "$RYUBING" ]; then
    echo "FATAL: Ryubing executable not found or not executable: $RYUBING" >&2
    exit 1
fi

if [ ! -f "$NRO" ]; then
    echo "FATAL: NRO not found: $NRO" >&2
    exit 1
fi

ARGS=(
    --no-gui
    --root-data-dir "$RYUJINX_DATA"
    --disable-file-logging
    --disable-docked-mode
    --ignore-missing-services
    --use-hypervisor false
    --enable-internet-connection
    "$NRO"
)

echo "Ryubing:  $RYUBING"
echo "Data dir: $RYUJINX_DATA"
echo "NRO:      $NRO"
echo "Log mode: $RYUBING_LOG_MODE"
echo "App log:  $RYUJINX_DATA/sdcard/switch/LunarNX/lunarnx.log"

case "$RYUBING_LOG_MODE" in
    discard)
        nohup "$RYUBING" "${ARGS[@]}" </dev/null >/dev/null 2>&1 &
        echo "PID:      $!"
        ;;
    filtered)
        LOG_FILE="/tmp/ryubing_nro_filtered_$$.log"
        nohup bash -c '
            "$1" "${@:2}" 2>&1 |
                awk '\''
                    /WaitSynchronization\(handleIndex: 0x00000000\) = InvalidHandle/ { next }
                    /SendSyncRequest\(\) = InvalidCurrentMemory/ { next }
                    /GetThreadName: Unknown ThreadType struct version/ { next }
                    { print; fflush() }
                '\'' > "$0"
        ' "$LOG_FILE" "$RYUBING" "${ARGS[@]}" </dev/null >/dev/null 2>&1 &
        echo "PID:      $!"
        echo "Ryubing filtered log: $LOG_FILE"
        ;;
    raw)
        LOG_FILE="/tmp/ryubing_nro_raw_$$.log"
        echo "WARNING: raw Ryubing logs can grow to multiple GB."
        nohup "$RYUBING" "${ARGS[@]}" </dev/null > "$LOG_FILE" 2>&1 &
        echo "PID:      $!"
        echo "Ryubing raw log: $LOG_FILE"
        ;;
    *)
        echo "FATAL: unknown RYUBING_LOG_MODE=$RYUBING_LOG_MODE (use discard, filtered, or raw)" >&2
        exit 1
        ;;
esac
