#!/usr/bin/env bash
# Helper for P0.3 soak / reconnect validation.
#
# Usage:
#   ./scripts/soak.sh                      # 5 minutes, videotestsrc
#   DURATION=10m ./scripts/soak.sh        # 10 minutes, videotestsrc
#   ./scripts/soak.sh --source rtsp       # 5 minutes, RTSP mode
#
# To validate RTSP reconnect, in another terminal during the soak:
#   docker stop p02-rtsp-pub
#   sleep 10
#   docker start p02-rtsp-pub

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DURATION="${DURATION:-5m}"
LOG_DIR="${LOG_DIR:-$ROOT/logs}"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/soak-$(date +%Y%m%d-%H%M%S).log"

parse_duration_seconds() {
    local raw="$1"
    if [[ "$raw" =~ ^([0-9]+)s$ ]]; then
        echo "${BASH_REMATCH[1]}"
    elif [[ "$raw" =~ ^([0-9]+)m$ ]]; then
        echo "$(( ${BASH_REMATCH[1]} * 60 ))"
    elif [[ "$raw" =~ ^([0-9]+)h$ ]]; then
        echo "$(( ${BASH_REMATCH[1]} * 3600 ))"
    elif [[ "$raw" =~ ^[0-9]+$ ]]; then
        echo "$raw"
    else
        echo "Unsupported DURATION format: $raw (use 300, 300s, 5m, 1h)" >&2
        exit 2
    fi
}

DURATION_SECONDS="$(parse_duration_seconds "$DURATION")"
OUTER_TIMEOUT_SECONDS=$(( DURATION_SECONDS + 20 ))

echo "P0.3 soak for $DURATION (${DURATION_SECONDS}s), logging to $LOG"
echo "Outer timeout guard: ${OUTER_TIMEOUT_SECONDS}s"

timeout --foreground --signal=INT "${OUTER_TIMEOUT_SECONDS}s" \
    "$HERE/run_in_container.sh" --duration-seconds "$DURATION_SECONDS" "$@" \
    2>&1 | tee "$LOG" || true

echo
echo "--- soak summary ---"
grep -E "CSV output:|Auto-quit:|Auto-quit fired|fps=.*src=|missing-t0|Frame arrived without t0 meta|Fatal pipeline error" "$LOG" | tail -30 || true
echo
CSV_LINE=$(grep -m1 "CSV output:" "$LOG" || true)
echo "csv: ${CSV_LINE:-'(not found)'}"
if grep -q "Frame arrived without t0 meta" "$LOG"; then
    echo "missing-t0: YES"
else
    echo "missing-t0: NO"
fi
if grep -q "Auto-quit fired" "$LOG"; then
    echo "auto-quit: observed"
else
    echo "auto-quit: not observed"
fi
echo "log: $LOG"
