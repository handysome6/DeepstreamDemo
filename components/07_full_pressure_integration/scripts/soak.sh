#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DURATION="${DURATION:-5m}"
LOG_DIR="${LOG_DIR:-$ROOT/logs}"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/soak-$(date +%Y%m%d-%H%M%S).log"

echo "Soak test for $DURATION, logging to $LOG"
timeout --foreground --signal=INT "$DURATION" \
    "$HERE/run_in_container.sh" "$@" 2>&1 | tee "$LOG" || true

echo
echo "--- soak summary ---"
grep -E "p07 status|stream LIVE|stream STALLED|pipeline error|pair DEGRADED|pair RECOVERED|stitch proof" "$LOG" | tail -80 || true
echo
LIVE_COUNT=$(grep -c "stream LIVE" "$LOG" || true)
STALL_COUNT=$(grep -c "stream STALLED" "$LOG" || true)
ERROR_COUNT=$(grep -c "pipeline error" "$LOG" || true)
DEGRADED_COUNT=$(grep -c "pair DEGRADED" "$LOG" || true)
RECOVERED_COUNT=$(grep -c "pair RECOVERED" "$LOG" || true)
echo "observed live transitions:   $LIVE_COUNT"
echo "observed stall transitions:  $STALL_COUNT"
echo "observed pipeline errors:    $ERROR_COUNT"
echo "observed pair degraded:      $DEGRADED_COUNT"
echo "observed pair recovered:     $RECOVERED_COUNT"
echo "log: $LOG"
