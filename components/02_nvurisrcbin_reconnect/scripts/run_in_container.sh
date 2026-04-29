#!/usr/bin/env bash
# Run the prototype inside the same container used to build it.
# Forwards X11 so the QOpenGLWidget window appears on the host.
#
# Usage:
#   ./scripts/run_in_container.sh                 # uses $RTSP_URL or default
#   RTSP_URL=rtsp://x:y@host/stream ./scripts/run_in_container.sh
#   ./scripts/run_in_container.sh --rtp udp       # extra args forwarded
#
# Default URI points at the local mediamtx test feed at cam0.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

IMAGE="${COMPONENT_IMAGE:-deepstream-demo/02-nvurisrcbin:latest}"
URI="${RTSP_URL:-rtsp://127.0.0.1:8554/cam0}"

if [[ ! -x "$ROOT/build/nvurisrcbin_reconnect" ]]; then
    echo "Binary not found. Run scripts/build_in_container.sh first." >&2
    exit 1
fi

if command -v xhost >/dev/null 2>&1; then
    xhost +local:root >/dev/null
    trap 'xhost -local:root >/dev/null 2>&1 || true' EXIT
fi

QT_GL_INT=xcb_glx
[[ "${ALLOW_EGL:-0}" == "1" ]] && QT_GL_INT=xcb_egl

# --net=host so the container can reach the local mediamtx at 127.0.0.1:8554
# without further network plumbing. For production cameras on the LAN this is
# also the simplest path.
docker run --rm --gpus all --net=host --entrypoint bash \
    -e DISPLAY="${DISPLAY:-:0}" \
    -e QT_XCB_GL_INTEGRATION="$QT_GL_INT" \
    -e ALLOW_EGL="${ALLOW_EGL:-}" \
    -e XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}" \
    -e GST_PLUGIN_PATH="/opt/nvidia/deepstream/deepstream-9.0/lib/gst-plugins:${GST_PLUGIN_PATH:-}" \
    -e LD_LIBRARY_PATH="/opt/nvidia/deepstream/deepstream-9.0/lib:${LD_LIBRARY_PATH:-}" \
    -v "${XAUTHORITY:-$HOME/.Xauthority}:${XAUTHORITY:-$HOME/.Xauthority}:ro" \
    -v /tmp/.X11-unix:/tmp/.X11-unix:rw \
    -v "$ROOT:/workspace" \
    -w /workspace \
    "$IMAGE" \
    -c "/workspace/build/nvurisrcbin_reconnect --uri '$URI' $*"
