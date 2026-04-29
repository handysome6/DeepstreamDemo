#!/usr/bin/env bash
# Runs the prototype against a real RTSP source. This is the bare NVDEC →
# NVMM/CUDA-device → GL texture path; no DeepStream elements yet. The point
# is to confirm that the dGPU GPU-only display path works with live,
# jittery, real-encoder input before we add any DeepStream complexity on top.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BIN="$ROOT/build/qt_eglimage_zerocopy"

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found at $BIN. Build first." >&2
    exit 1
fi

URL="${1:-rtsp://127.0.0.1:8554/test}"
CODEC="${2:-h264}"   # h264 or h265

case "$CODEC" in
    h264) DEPAY=rtph264depay; PARSE=h264parse; DEC=nvv4l2decoder ;;
    h265) DEPAY=rtph265depay; PARSE=h265parse; DEC=nvv4l2decoder ;;
    *) echo "Unknown codec: $CODEC (use h264 or h265)" >&2; exit 2 ;;
esac

export QT_XCB_GL_INTEGRATION=xcb_glx
# export GST_DEBUG=3

PIPELINE="rtspsrc location=$URL protocols=tcp latency=0 drop-on-latency=true ! "
PIPELINE+="$DEPAY ! $PARSE ! $DEC ! "
PIPELINE+='nvvideoconvert ! '
PIPELINE+='video/x-raw(memory:NVMM),format=RGBA ! '
PIPELINE+='appsink name=sink'

echo "Pipeline: $PIPELINE"
exec "$BIN" "$PIPELINE"
