#!/usr/bin/env bash
# Helper for the success-criteria soak test. Captures stdout to a log file so
# the 30-min run can be reviewed after the fact, and prints a summary at the
# end.
#
# Usage:
#   ./scripts/soak.sh                       # 30 minutes, default URI
#   DURATION=60m ./scripts/soak.sh          # 60-minute soak
#   RTSP_URL=rtsp://... ./scripts/soak.sh
#
# To validate the "manual disconnect 3 times" criterion, in another terminal:
#   docker stop p02-rtsp-pub
#   sleep 20
#   docker start p02-rtsp-pub
# Repeat until the binary's stdout has logged at least 3 reconnects.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DURATION="${DURATION:-30m}"
LOG_DIR="${LOG_DIR:-$ROOT/logs}"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/soak-$(date +%Y%m%d-%H%M%S).log"

echo "Soak test for $DURATION, logging to $LOG"
timeout --foreground --signal=INT "$DURATION" \
    "$HERE/run_in_container.sh" "$@" 2>&1 | tee "$LOG" || true

echo
echo "--- soak summary ---"
grep -E "fps=|stream LIVE|stream STALLED|reconnects=" "$LOG" | tail -20
echo
RECONNECTS=$(grep -c "stream LIVE" "$LOG" || true)
STALLS=$(grep -c "stream STALLED" "$LOG" || true)
echo "observed live transitions:  $RECONNECTS"
echo "observed stall transitions: $STALLS"
echo "log: $LOG"
