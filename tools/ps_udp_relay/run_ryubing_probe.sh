#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
RYUJINX_APP=${LUNARNX_RYUJINX_APP:-/tmp/ryubing-canary-1.3.333/Ryujinx.app}
DATA_DIR=${LUNARNX_RYUJINX_DATA:-$HOME/work/self/ryujinx-data}
NRO=${LUNARNX_PS_PROBE_NRO:-$ROOT/build/switch-psprobe/LunarNXPsSessionProbe.nro}
LOG="$DATA_DIR/sdcard/switch/LunarNX/ps_session_probe.log"
ATTEMPT_LOG_DIR="$DATA_DIR/sdcard/switch/LunarNX/probe-attempts"
RELAY_LOG=${LUNARNX_PS_RELAY_LOG:-}

if [ ! -f "$NRO" ]; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "PS probe NRO not found and Docker is unavailable: $NRO" >&2
        exit 2
    fi
    echo "PS probe NRO not found; building it in devkitA64 Docker" >&2
    docker run --rm --platform linux/amd64 \
        -v "$ROOT:/work" -w /work \
        devkitpro/devkita64:20251117 bash -lc '
            set -e
            export DEVKITPRO=/opt/devkitpro
            export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
            make -f Makefile.switch.psprobe -j$(nproc)
        '
fi
if pgrep -f "$RYUJINX_APP/Contents/MacOS/Ryujinx --no-gui" >/dev/null 2>&1; then
    echo "Ryubing is already running; close it before starting the probe" >&2
    exit 2
fi

attempt=1
last_result="TIMEOUT"
while [ "$attempt" -le 4 ]; do
    python3 "$ROOT/tools/ps_udp_relay/refresh_psn_token.py" --data-dir "$DATA_DIR"
    rm -f "$LOG"
    mkdir -p "$ATTEMPT_LOG_DIR"
    attempt_stamp=$(date -u +%Y%m%dT%H%M%SZ)
    relay_start_line=1
    if [ -n "$RELAY_LOG" ] && [ -f "$RELAY_LOG" ]; then
        relay_start_line=$(( $(wc -l < "$RELAY_LOG") + 1 ))
    fi

    open -na "$RYUJINX_APP" --args \
        --no-gui \
        --root-data-dir "$DATA_DIR" \
        --disable-file-logging \
        --disable-docked-mode \
        --ignore-missing-services \
        --use-hypervisor false \
        --enable-internet-connection \
        "$NRO"

    i=0
    while [ "$i" -lt 360 ]; do
        if [ -f "$LOG" ] && grep -q '^PROBE_RESULT ' "$LOG"; then
            last_result=$(sed -n 's/^PROBE_RESULT //p' "$LOG" | tail -1)
            break
        fi
        sleep 0.5
        i=$((i + 1))
    done
    if [ -f "$LOG" ]; then
        cp "$LOG" "$ATTEMPT_LOG_DIR/ps_session_probe.attempt-${attempt}.${attempt_stamp}.log"
    fi
    if [ -n "$RELAY_LOG" ] && [ -f "$RELAY_LOG" ]; then
        tail -n "+$relay_start_line" "$RELAY_LOG" \
            > "$ATTEMPT_LOG_DIR/ps_session_probe.attempt-${attempt}.${attempt_stamp}.relay.log"
    fi
    tail -40 "$LOG" 2>/dev/null || true
    probe_pid=$(pgrep -f "$NRO" | head -1 || true)
    if [ -n "$probe_pid" ]; then
        kill "$probe_pid" 2>/dev/null || true
    fi
    if [ "$last_result" = PASS ]; then
        exit 0
    fi
    if [ "$attempt" -lt 4 ]; then
        sleep $((attempt * 3))
    fi
    attempt=$((attempt + 1))
done

echo "PS probe failed after 4 attempts: $last_result" >&2
exit 1
