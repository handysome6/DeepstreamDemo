#!/usr/bin/env bash
# Overnight one-shot 30-min full-pressure soak for P0.7.
#
# Designed to be invoked by a single cron entry tagged
# `overnight_soak_one_shot`. The script:
#   1. Removes its own cron entry first (so a mid-run crash will not loop
#      again the following night).
#   2. Runs the 30-min `--stage full` soak via scripts/soak.sh.
#   3. Captures a parallel nvidia-smi VRAM sampler at 5 s.
#   4. Parses the soak log and writes a self-contained summary to
#      logs/overnight-summary-<ts>.txt.
#
# Designed to survive SSH session close — cron detaches from any tty and
# the script does not rely on the invoking shell's environment. Ensure
# the local mediamtx + ffmpeg-pub containers (cam0..cam5) are running
# before 02:00; the script does not attempt to start them.

set -u

ROOT="/home/andy/workspace/DeepstreamDemo/components/07_full_pressure_integration"
TS="$(date +%Y%m%d-%H%M%S)"
LOG_DIR="$ROOT/logs"
SUMMARY="$LOG_DIR/overnight-summary-$TS.txt"
mkdir -p "$LOG_DIR"

log() { echo "[overnight_soak $(date '+%F %T')] $*" | tee -a "$SUMMARY"; }

# ------------------------------------------------------------------------
# Step 1: self-remove the cron entry FIRST. If anything below fails, the
# entry must already be gone so the soak does not retry tomorrow.
# ------------------------------------------------------------------------
if crontab -l 2>/dev/null | grep -q 'overnight_soak_one_shot'; then
    crontab -l 2>/dev/null | grep -v 'overnight_soak_one_shot' | crontab -
    log "removed cron entry tagged overnight_soak_one_shot"
else
    log "no cron entry to remove (manual invocation?)"
fi

# ------------------------------------------------------------------------
# Step 2: predictable env for non-tty cron context. Cron's env is empty
# beyond a minimal PATH; everything that the docker harness expects to
# resolve must be exported here.
# ------------------------------------------------------------------------
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME="/home/andy"
export USER="andy"
export DISPLAY=":0"
# The local X server already has `SI:localuser:andy` granted (verified at
# install time). The container runs as root, so we extend xhost below.
unset XAUTHORITY

log "starting overnight 30-min soak"
log "ROOT=$ROOT  TS=$TS  PID=$$"
nvidia-smi -L 2>&1 | tee -a "$SUMMARY" || true
PRE_VRAM=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1 | tr -d ' ')
log "pre-soak VRAM: ${PRE_VRAM} MiB"

# ------------------------------------------------------------------------
# Step 3: verify the local mediamtx + ffmpeg-pub lab is running. Per user
# instruction, do NOT attempt to start it — just report and abort if missing.
# ------------------------------------------------------------------------
LAB_OK=true
for c in deepstreampoc-rtsp-server \
         deepstreampoc-rtsp-pub-0 deepstreampoc-rtsp-pub-1 \
         deepstreampoc-rtsp-pub-2 deepstreampoc-rtsp-pub-3 \
         deepstreampoc-rtsp-pub-4 deepstreampoc-rtsp-pub-5; do
    if ! docker ps --format '{{.Names}}' | grep -qx "$c"; then
        log "WARN: container $c is NOT running"
        LAB_OK=false
    fi
done

if [[ "$LAB_OK" != "true" ]]; then
    log "ABORT: required mediamtx + ffmpeg publisher containers are not all running."
    log "(skipping soak per user instruction; not attempting to start them)"
    exit 2
fi

# X11 access for the container's root user. Idempotent.
xhost +local:root 2>&1 | tee -a "$SUMMARY" || log "xhost grant failed (continuing — /tmp/.X11-unix may still allow)"

# ------------------------------------------------------------------------
# Step 4: background VRAM sampler at 5 s
# ------------------------------------------------------------------------
VRAM_CSV="$LOG_DIR/overnight-vram-$TS.csv"
echo "epoch_s,memory_used_MiB" > "$VRAM_CSV"
(
    while true; do
        ts=$(date +%s)
        mem=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1 | tr -d ' ')
        echo "$ts,$mem" >> "$VRAM_CSV"
        sleep 5
    done
) &
SAMPLER_PID=$!
log "VRAM sampler PID=$SAMPLER_PID writing to $VRAM_CSV"

# Make sure we kill the sampler on any exit path.
cleanup() {
    if kill -0 "$SAMPLER_PID" 2>/dev/null; then
        kill "$SAMPLER_PID" 2>/dev/null || true
        wait "$SAMPLER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# ------------------------------------------------------------------------
# Step 5: run the soak
# ------------------------------------------------------------------------
cd "$ROOT" || { log "ABORT: cd $ROOT failed"; exit 3; }

SOAK_OUT="$LOG_DIR/overnight-soak-$TS.stdout.log"
log "kicking soak: DURATION=30m ./scripts/soak.sh --stage full"
log "(stdout teed to $SOAK_OUT; soak.sh writes its own logs/soak-*.log)"

SOAK_START=$(date +%s)
DURATION=30m ./scripts/soak.sh --stage full > "$SOAK_OUT" 2>&1
SOAK_EXIT=$?
SOAK_END=$(date +%s)
SOAK_RUNTIME=$((SOAK_END - SOAK_START))
log "soak.sh exited rc=$SOAK_EXIT after ${SOAK_RUNTIME}s"

cleanup
trap - EXIT INT TERM

POST_VRAM=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1 | tr -d ' ')
log "post-soak VRAM: ${POST_VRAM} MiB"

# ------------------------------------------------------------------------
# Step 6: parse the soak log and the VRAM CSV
# ------------------------------------------------------------------------
SOAK_LOG=$(ls -1t "$LOG_DIR"/soak-*.log 2>/dev/null | head -1)

LIVE_COUNT=0; STALL_COUNT=0; ERR_COUNT=0
DEGRADED_COUNT=0; RECOVERED_COUNT=0
LAST_STATUS="(no p07 status line found)"

if [[ -n "$SOAK_LOG" && -f "$SOAK_LOG" ]]; then
    LIVE_COUNT=$(grep -c "stream LIVE" "$SOAK_LOG" || true)
    STALL_COUNT=$(grep -c "stream STALLED" "$SOAK_LOG" || true)
    ERR_COUNT=$(grep -c "pipeline error" "$SOAK_LOG" || true)
    DEGRADED_COUNT=$(grep -c "pair DEGRADED" "$SOAK_LOG" || true)
    RECOVERED_COUNT=$(grep -c "pair RECOVERED" "$SOAK_LOG" || true)
    LAST_STATUS=$(grep "p07 status" "$SOAK_LOG" | tail -1)
    log "parsed soak log: $SOAK_LOG"
else
    log "WARN: could not find soak log under $LOG_DIR"
fi

# VRAM band over the soak window (skip first 60 s ramp)
VRAM_STATS="(no samples)"
if [[ -f "$VRAM_CSV" ]]; then
    VRAM_STATS=$(awk -F, 'NR>13 && $2 ~ /^[0-9]+$/' "$VRAM_CSV" \
        | sort -t, -k2 -n \
        | awk -F, 'BEGIN{c=0} {a[c++]=$2; s+=$2} END{
            if(c==0){print "no samples"; exit}
            printf "samples=%d  min=%d MiB  median=%d MiB  mean=%.1f MiB  max=%d MiB  spread=%d MiB",
                   c, a[0], a[int(c/2)], s/c, a[c-1], a[c-1]-a[0]
        }')
fi

# STALL events (not just the count) — show line context for each
STALL_LINES="(none)"
if [[ -n "$SOAK_LOG" && -f "$SOAK_LOG" ]]; then
    if [[ "$STALL_COUNT" -gt 0 ]]; then
        STALL_LINES=$(grep "STALLED" "$SOAK_LOG" | head -10)
    fi
fi

# ------------------------------------------------------------------------
# Step 7: write the verdict
# ------------------------------------------------------------------------
log "============================================================"
log "P0.7 OVERNIGHT SOAK SUMMARY"
log "============================================================"
log ""
log "runtime: ${SOAK_RUNTIME} s (target: 1800)"
log "soak.sh exit code: $SOAK_EXIT  (124 = expected SIGINT from timeout, 0 = clean Qt quit)"
log ""
log "counters from soak log:"
log "  stream LIVE transitions:   $LIVE_COUNT"
log "  stream STALLED transitions: $STALL_COUNT"
log "  pipeline errors:           $ERR_COUNT"
log "  pair DEGRADED events:      $DEGRADED_COUNT"
log "  pair RECOVERED events:     $RECOVERED_COUNT"
log ""
log "VRAM:  pre=${PRE_VRAM} MiB  post=${POST_VRAM} MiB"
log "VRAM band (skipping first 60 s ramp): $VRAM_STATS"
log ""
log "last p07 status line:"
log "  $LAST_STATUS"
log ""
log "STALL event excerpts (first 10):"
while IFS= read -r line; do log "  $line"; done <<< "$STALL_LINES"
log ""
log "============================================================"
log "verdict per README success criteria:"
if [[ "$SOAK_EXIT" -eq 0 || "$SOAK_EXIT" -eq 124 ]]; then
    log "  [PASS] no crash"
else
    log "  [FAIL] no crash — soak.sh exited with $SOAK_EXIT"
fi
if [[ "$ERR_COUNT" -eq 0 ]]; then
    log "  [PASS] no pipeline errors"
else
    log "  [FAIL] no pipeline errors — saw $ERR_COUNT"
fi
# Steady-state stalls: 04/05/06 baseline allow 0–1 startup stalls + maybe
# 1–2 transient publisher hiccups in 30 min. >5 deserves human review.
if [[ "$STALL_COUNT" -le 5 ]]; then
    log "  [PASS] steady-state stalls ≤ 5  ($STALL_COUNT)"
else
    log "  [REVIEW] steady-state stalls = $STALL_COUNT (>5 — investigate)"
fi
log ""
log "files written:"
log "  summary    : $SUMMARY"
log "  soak log   : ${SOAK_LOG:-<missing>}"
log "  soak stdout: $SOAK_OUT"
log "  vram csv   : $VRAM_CSV"
log "============================================================"

exit 0
