#!/usr/bin/env bash
# Runs the prototype against a pure synthetic NVMM source (no camera).
# Use this as the first sanity test: if this does not show a moving ball
# pattern with low latency, the EGLImage path itself is broken and there is
# no point trying RTSP yet.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BIN="$ROOT/build/qt_eglimage_zerocopy"

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found at $BIN. Build first:" >&2
    echo "  cmake -S $ROOT -B $ROOT/build -DCMAKE_BUILD_TYPE=Release" >&2
    echo "  cmake --build $ROOT/build -j" >&2
    exit 1
fi

# Required so Qt and NvBufSurface share the same EGLDisplay.
export QT_XCB_GL_INTEGRATION=xcb_egl

# Uncomment to debug GStreamer plugin negotiation issues.
# export GST_DEBUG=3

PIPELINE='videotestsrc is-live=true pattern=ball ! '
PIPELINE+='video/x-raw,width=1920,height=1080,framerate=30/1 ! '
PIPELINE+='nvvideoconvert ! '
PIPELINE+='video/x-raw(memory:NVMM),format=NV12 ! '
PIPELINE+='appsink name=sink'

exec "$BIN" "$PIPELINE"
