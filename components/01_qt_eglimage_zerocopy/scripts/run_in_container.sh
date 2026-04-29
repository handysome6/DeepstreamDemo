#!/usr/bin/env bash
# Run the prototype inside the same Qt6+DeepStream container used to build it.
# Forwards X11 display so the QOpenGLWidget window appears on the host.
#
# Usage:
#   ./scripts/run_in_container.sh                       # videotestsrc
#   ./scripts/run_in_container.sh "<gst pipeline str>"  # custom pipeline
#   ./scripts/run_in_container.sh rtsp                  # RTSP preset (URL via $RTSP_URL)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

IMAGE="${COMPONENT_IMAGE:-deepstream-demo/01-qt-eglimage:latest}"

if [[ ! -x "$ROOT/build/qt_eglimage_zerocopy" ]]; then
    echo "Binary not found. Run scripts/build_in_container.sh first." >&2
    exit 1
fi

VIDEOTESTSRC_PIPELINE='videotestsrc is-live=true pattern=ball ! '
VIDEOTESTSRC_PIPELINE+='video/x-raw,width=1920,height=1080,framerate=30/1 ! '
VIDEOTESTSRC_PIPELINE+='nvvideoconvert ! '
VIDEOTESTSRC_PIPELINE+='video/x-raw(memory:NVMM),format=RGBA ! '
VIDEOTESTSRC_PIPELINE+='appsink name=sink'

case "${1:-videotestsrc}" in
    videotestsrc) PIPELINE="$VIDEOTESTSRC_PIPELINE" ;;
    rtsp)
        URL="${RTSP_URL:?set RTSP_URL=rtsp://...}"
        PIPELINE="rtspsrc location=$URL latency=0 drop-on-latency=true ! "
        PIPELINE+="rtph264depay ! h264parse ! nvh264dec ! "
        PIPELINE+='nvvideoconvert ! '
        PIPELINE+='video/x-raw(memory:NVMM),format=RGBA ! '
        PIPELINE+='appsink name=sink'
        ;;
    *) PIPELINE="$1" ;;
esac

# Allow the container's root user to talk to the host X server.
# Removed at exit so we don't leave the policy permanently relaxed.
if command -v xhost >/dev/null 2>&1; then
    xhost +local:root >/dev/null
    trap 'xhost -local:root >/dev/null 2>&1 || true' EXIT
fi

QT_GL_INT=xcb_glx
[[ "${ALLOW_EGL:-0}" == "1" ]] && QT_GL_INT=xcb_egl

docker run --rm --gpus all --net=host --entrypoint bash \
    -e DISPLAY="${DISPLAY:-:0}" \
    -e QT_XCB_GL_INTEGRATION="$QT_GL_INT" \
    -e ALLOW_EGL="${ALLOW_EGL:-}" \
    -e XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}" \
    -v "${XAUTHORITY:-$HOME/.Xauthority}:${XAUTHORITY:-$HOME/.Xauthority}:ro" \
    -v /tmp/.X11-unix:/tmp/.X11-unix:rw \
    -v "$ROOT:/workspace" \
    -w /workspace \
    "$IMAGE" \
    -c "/workspace/build/qt_eglimage_zerocopy '$PIPELINE'"
