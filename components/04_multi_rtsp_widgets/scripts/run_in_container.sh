#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

IMAGE="${COMPONENT_IMAGE:-deepstream-demo/04-multi-rtsp-widgets:latest}"
DEFAULT_URI="${RTSP_URL:-rtsp://127.0.0.1:8554/p02cam}"
BINARY="$ROOT/build/multi_rtsp_widgets"

if [[ ! -x "$BINARY" ]]; then
    echo "Binary not found. Run scripts/build_in_container.sh first." >&2
    exit 1
fi

NEEDS_XHOST_CLEANUP=0
if command -v xhost >/dev/null 2>&1; then
    xhost +local:root >/dev/null
    NEEDS_XHOST_CLEANUP=1
fi

QT_GL_INT=xcb_glx
[[ "${ALLOW_EGL:-0}" == "1" ]] && QT_GL_INT=xcb_egl

args=("$@")
has_uri=0
for arg in "$@"; do
    if [[ "$arg" == "--uri" || "$arg" == "-u" ]]; then
        has_uri=1
        break
    fi
done
if [[ $has_uri -eq 0 ]]; then
    args=(--uri "$DEFAULT_URI" --uri "$DEFAULT_URI" "${args[@]}")
fi

CONTAINER_NAME="p04-multi-rtsp-widgets-$(date +%Y%m%d-%H%M%S)-$$"
DOCKER_PID=""

cleanup() {
    local rc=$?
    if [[ -n "$DOCKER_PID" ]] && kill -0 "$DOCKER_PID" >/dev/null 2>&1; then
        docker stop --signal=INT --time 5 "$CONTAINER_NAME" >/dev/null 2>&1 || true
        sleep 1
        if kill -0 "$DOCKER_PID" >/dev/null 2>&1; then
            docker kill "$CONTAINER_NAME" >/dev/null 2>&1 || true
        fi
    fi
    if [[ "$NEEDS_XHOST_CLEANUP" == "1" ]]; then
        xhost -local:root >/dev/null 2>&1 || true
    fi
    return $rc
}
trap cleanup EXIT INT TERM

docker run --rm --name "$CONTAINER_NAME" --gpus all --net=host \
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
    /workspace/build/multi_rtsp_widgets "${args[@]}" &
DOCKER_PID=$!
wait "$DOCKER_PID"
DOCKER_PID=""
