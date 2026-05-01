#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

IMAGE="${COMPONENT_IMAGE:-deepstream-demo/06-dual-4k-stitch-restream:latest}"
DEFAULT_URI="${RTSP_URL:-rtsp://127.0.0.1:8554/p02cam}"
BINARY="$ROOT/build/dual_4k_stitch_restream"

if [[ ! -x "$BINARY" ]]; then
    echo "Binary not found. Run scripts/build_in_container.sh first." >&2
    exit 1
fi

DISPLAY_VALUE="${DISPLAY:-:0}"
XAUTHORITY_VALUE="${XAUTHORITY:-$HOME/.Xauthority}"

NEEDS_XHOST_CLEANUP=0
if command -v xhost >/dev/null 2>&1; then
    if DISPLAY="$DISPLAY_VALUE" xhost +local:root >/dev/null 2>&1; then
        NEEDS_XHOST_CLEANUP=1
    fi
fi

QT_GL_INT=xcb_glx
[[ "${ALLOW_EGL:-0}" == "1" ]] && QT_GL_INT=xcb_egl

args=("$@")
has_top=0
has_bottom=0
for arg in "$@"; do
    [[ "$arg" == "--top-uri" ]] && has_top=1
    [[ "$arg" == "--bottom-uri" ]] && has_bottom=1
done
if [[ $has_top -eq 0 ]]; then
    args=(--top-uri "$DEFAULT_URI" "${args[@]}")
fi
if [[ $has_bottom -eq 0 ]]; then
    args=(--bottom-uri "$DEFAULT_URI" "${args[@]}")
fi

CONTAINER_NAME="${COMPONENT_CONTAINER_NAME:-p06-dual-stitch-$(date +%Y%m%d-%H%M%S)-$$}"

cleanup_container() {
    docker stop "$CONTAINER_NAME" >/dev/null 2>&1 || true
    if [[ "$NEEDS_XHOST_CLEANUP" == "1" ]]; then
        DISPLAY="$DISPLAY_VALUE" xhost -local:root >/dev/null 2>&1 || true
    fi
}
trap cleanup_container EXIT INT TERM

docker run --rm --init --gpus all --net=host \
    --name "$CONTAINER_NAME" \
    --entrypoint /workspace/build/dual_4k_stitch_restream \
    -e DISPLAY="$DISPLAY_VALUE" \
    -e QT_XCB_GL_INTEGRATION="$QT_GL_INT" \
    -e ALLOW_EGL="${ALLOW_EGL:-}" \
    -e XAUTHORITY="$XAUTHORITY_VALUE" \
    -e GST_PLUGIN_PATH="/opt/nvidia/deepstream/deepstream-9.0/lib/gst-plugins:${GST_PLUGIN_PATH:-}" \
    -e LD_LIBRARY_PATH="/opt/nvidia/deepstream/deepstream-9.0/lib:${LD_LIBRARY_PATH:-}" \
    -v "$XAUTHORITY_VALUE:$XAUTHORITY_VALUE:ro" \
    -v /tmp/.X11-unix:/tmp/.X11-unix:rw \
    -v "$ROOT:/workspace" \
    -w /workspace \
    "$IMAGE" \
    "${args[@]}" &
DOCKER_RUN_PID=$!
set +e
wait "$DOCKER_RUN_PID"
STATUS=$?
set -e
exit "$STATUS"
