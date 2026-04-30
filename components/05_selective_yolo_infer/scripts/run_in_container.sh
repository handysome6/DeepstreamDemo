#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

IMAGE="${COMPONENT_IMAGE:-deepstream-demo/05-selective-yolo:latest}"
DEFAULT_URI="${RTSP_URL:-rtsp://127.0.0.1:8554/p02cam}"
BINARY="$ROOT/build/selective_yolo_infer"

if [[ ! -x "$BINARY" ]]; then
    echo "Binary not found. Run scripts/build_in_container.sh first." >&2
    exit 1
fi

if command -v xhost >/dev/null 2>&1; then
    xhost +local:root >/dev/null
    trap 'xhost -local:root >/dev/null 2>&1 || true' EXIT
fi

QT_GL_INT=xcb_glx
[[ "${ALLOW_EGL:-0}" == "1" ]] && QT_GL_INT=xcb_egl

# If the caller did not pass --uri, default to two panels with the same local
# RTSP feed and the first panel running infer. That gives the smoke test a
# meaningful "selective" signal: one YOLO panel + one raw panel side by side.
args=("$@")
has_uri=0; has_infer_flag=0
for arg in "$@"; do
    case "$arg" in
        --uri|-u) has_uri=1 ;;
        --infer|-i|--infer-all) has_infer_flag=1 ;;
    esac
done
if [[ $has_uri -eq 0 ]]; then
    args=(--uri "$DEFAULT_URI" --uri "$DEFAULT_URI" "${args[@]}")
fi
if [[ $has_infer_flag -eq 0 ]]; then
    args=("${args[@]}" --infer 1)
fi

docker run --rm --gpus all --net=host \
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
    /workspace/build/selective_yolo_infer "${args[@]}"
