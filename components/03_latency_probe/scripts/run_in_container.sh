#!/usr/bin/env bash
# Run latency_probe inside the same container used to build it.
# Forwards X11 so the QOpenGLWidget window appears on the host.
#
# Usage:
#   ./scripts/run_in_container.sh                                # videotestsrc, vsync off
#   ./scripts/run_in_container.sh --source rtsp                  # default RTSP feed
#   ./scripts/run_in_container.sh --source rtsp --uri rtsp://x/y
#   ./scripts/run_in_container.sh --vsync on                     # vsync sanity run
#
# Any extra flags are forwarded to the binary.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

IMAGE="${COMPONENT_IMAGE:-deepstream-demo/03-latency-probe:latest}"

if [[ ! -x "$ROOT/build/latency_probe" ]]; then
    echo "Binary not found. Run scripts/build_in_container.sh first." >&2
    exit 1
fi

mkdir -p "$ROOT/logs"

NEEDS_XHOST_CLEANUP=0
if command -v xhost >/dev/null 2>&1; then
    xhost +local:root >/dev/null
    NEEDS_XHOST_CLEANUP=1
fi

QT_GL_INT=xcb_glx
[[ "${ALLOW_EGL:-0}" == "1" ]] && QT_GL_INT=xcb_egl

# vblank_mode=0 / __GL_SYNC_TO_VBLANK=0 force the GL driver to ignore vsync
# (Mesa and NVIDIA respectively). They are useful as belt-and-suspenders for
# the default --vsync off case to defeat compositor overrides, but they
# *override* QSurfaceFormat::setSwapInterval(1), so passing them in the
# --vsync on case silently breaks the vsync sanity test in the README.
# Conditionally apply only when the user is NOT asking for vsync.
VSYNC_ENV=()
if [[ " $* " == *" --vsync on"* ]] || [[ " $* " == *" --vsync=on"* ]]; then
    echo "[run_in_container] --vsync on detected; leaving driver vsync env vars unset."
else
    VSYNC_ENV+=(-e vblank_mode=0 -e __GL_SYNC_TO_VBLANK=0)
fi

CONTAINER_NAME="p03-latency-probe-$(date +%Y%m%d-%H%M%S)-$$"
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

# --net=host so the container can reach the local mediamtx at 127.0.0.1:8554.
docker run --rm --name "$CONTAINER_NAME" --gpus all --net=host \
    -e DISPLAY="${DISPLAY:-:0}" \
    -e QT_XCB_GL_INTEGRATION="$QT_GL_INT" \
    -e ALLOW_EGL="${ALLOW_EGL:-}" \
    -e XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}" \
    -e GST_PLUGIN_PATH="/opt/nvidia/deepstream/deepstream-9.0/lib/gst-plugins:${GST_PLUGIN_PATH:-}" \
    -e LD_LIBRARY_PATH="/opt/nvidia/deepstream/deepstream-9.0/lib:${LD_LIBRARY_PATH:-}" \
    "${VSYNC_ENV[@]}" \
    -v "${XAUTHORITY:-$HOME/.Xauthority}:${XAUTHORITY:-$HOME/.Xauthority}:ro" \
    -v /tmp/.X11-unix:/tmp/.X11-unix:rw \
    -v "$ROOT:/workspace" \
    -w /workspace \
    "$IMAGE" \
    /workspace/build/latency_probe "$@" &
DOCKER_PID=$!
wait "$DOCKER_PID"
DOCKER_PID=""
