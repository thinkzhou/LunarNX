#!/bin/bash
# Run an NRO in Ryujinx Headless and report the result.
#
# Prerequisites (one-time):
#   Place prod.keys in the Ryujinx data directory:
#     {RYUJINX_DATA}/system/prod.keys
#   Firmware NCA files (optional but recommended):
#     {RYUJINX_DATA}/bis/system/Contents/registered/*.nca
#
# Usage:
#   ./scripts/run_nro_test.sh [path/to/name.nro] [timeout_seconds]
#   RYUJINX_ENABLE_INTERNET=1 ./scripts/run_nro_test.sh [path/to/name.nro] [timeout_seconds]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

RYUJINX="${RYUJINX:-$PROJECT_DIR/../ryujinx/publish_headless/Ryujinx.Headless.SDL2}"
RYUJINX_DATA="${RYUJINX_DATA:-$PROJECT_DIR/../ryujinx-data}"
RYUJINX_ENABLE_INTERNET="${RYUJINX_ENABLE_INTERNET:-0}"
TIMEOUT="${2:-15}"

NRO="${1:-$PROJECT_DIR/build/switch-smoke/LunarNXSmoke.nro}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "============================================"
echo " NRO Smoke Test Runner"
echo "============================================"
echo "NRO:        $NRO"
echo "Ryujinx:    $RYUJINX"
echo "Data dir:   $RYUJINX_DATA"
echo "Internet:   $RYUJINX_ENABLE_INTERNET"
echo "Timeout:    ${TIMEOUT}s"
echo ""

# --- Pre-flight checks ---

if [ ! -f "$RYUJINX" ]; then
    echo -e "${RED}FATAL:${NC} Ryujinx binary not found at $RYUJINX"
    echo "Build it first, or set RYUJINX env var."
    exit 1
fi

if [ ! -f "$NRO" ]; then
    echo -e "${RED}FATAL:${NC} NRO not found: $NRO"
    echo "Build it first: make -f Makefile.switch.smoke"
    exit 1
fi

if [ ! -f "$RYUJINX_DATA/system/prod.keys" ]; then
    echo -e "${YELLOW}WARNING:${NC} prod.keys not found in $RYUJINX_DATA/system/"
    echo "Run: ./scripts/setup_ryujinx_firmware.sh"
    echo ""
fi

# --- Run ---

LOG_FILE="/tmp/ryujinx_nro_test_$$.log"

echo "Launching Ryujinx..."

RYUJINX_ARGS=(
    --root-data-dir "$RYUJINX_DATA"
    --disable-file-logging
    --enable-debug-logs
    --disable-docked-mode
    --ignore-missing-services
    --use-hypervisor false
)

if [ "$RYUJINX_ENABLE_INTERNET" = "1" ] || [ "$RYUJINX_ENABLE_INTERNET" = "true" ]; then
    RYUJINX_ARGS+=(--enable-internet-connection)
fi

# macOS doesn't have the 'timeout' command; implement timeout manually
"$RYUJINX" "${RYUJINX_ARGS[@]}" "$NRO" > "$LOG_FILE" 2>&1 &
RYUJINX_PID=$!

echo "PID: $RYUJINX_PID"
echo ""

# Wait up to TIMEOUT seconds; check if process exits early
ELAPSED=0
while [ $ELAPSED -lt $TIMEOUT ]; do
    if ! kill -0 $RYUJINX_PID 2>/dev/null; then
        # Process exited
        wait $RYUJINX_PID 2>/dev/null
        EXIT_CODE=$?
        echo "Ryujinx exited after ${ELAPSED}s (code $EXIT_CODE)"
        break
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

if kill -0 $RYUJINX_PID 2>/dev/null; then
    # Still running — kill it (Ryujinx doesn't auto-exit when NRO terminates)
    kill $RYUJINX_PID 2>/dev/null || true
    wait $RYUJINX_PID 2>/dev/null || true
    EXIT_CODE=124
    echo "Killed after ${TIMEOUT}s timeout"
fi

echo ""
echo "============================================"
echo " Result"
echo "============================================"

# Analyse the log
if grep -qi "Application Loaded.*LunarNX" "$LOG_FILE"; then
    echo -e "${GREEN}PASS:${NC} NRO was loaded by Ryujinx."
    LOADED=true
else
    LOADED=false
fi

if grep -qi "InvalidCurrentMemory\|SendSyncRequest.*InvalidHandle\|WaitSynchronization.*InvalidHandle" "$LOG_FILE"; then
    echo -e "${YELLOW}NOTE:${NC} Ryujinx reported non-fatal HLE handle/memory warnings."
    HAS_HLE_WARNING=true
else
    HAS_HLE_WARNING=false
fi

if grep -qi "Guest network access disabled\|DNS Blocked" "$LOG_FILE"; then
    echo -e "${YELLOW}NOTE:${NC} Guest network was blocked. Re-run with RYUJINX_ENABLE_INTERNET=1 for network tests."
    HAS_NETWORK_BLOCK=true
else
    HAS_NETWORK_BLOCK=false
fi

if grep -qi "Fatal error\|Unhandled exception\|Crash\|Segfault" "$LOG_FILE"; then
    echo -e "${YELLOW}WARN:${NC} Error/crash detected during execution."
    HAS_ERROR=true
else
    HAS_ERROR=false
fi

echo ""
echo "Key events from log:"
grep -iE "Application Load|Loading as|Homebrew|Firmware Version|Gpu Print|InvalidCurrentMemory|InvalidHandle|Error|Exit|Crash|Exception|Stubbed" "$LOG_FILE" 2>/dev/null | head -20 || echo "(no key events)"

echo ""
echo "Full log: $LOG_FILE"

# Return 0 on success
if $LOADED && ! $HAS_ERROR; then
    exit 0
else
    exit 1
fi
