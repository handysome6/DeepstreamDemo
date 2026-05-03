#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

IMAGE="${COMPONENT_IMAGE:-deepstream-demo/07-full-pressure:latest}"
BINARY="$ROOT/build/full_pressure_integration"

if [[ ! -x "$BINARY" ]]; then
    echo "Binary not found. Run scripts/build_in_container.sh first." >&2
    exit 1
fi

# Default URI roster: the existing local mediamtx lab from 04/06 — cam0..cam5.
# 07 duplicates these onto 7 1080p slots when the user does not override.
# The 4K slots default to cam4/cam5 which 06 uses for its 4K stitch
# validation. Override any of these with the explicit flags below.
DEFAULT_1080P=(
    "rtsp://127.0.0.1:8554/cam0"
    "rtsp://127.0.0.1:8554/cam1"
    "rtsp://127.0.0.1:8554/cam2"
    "rtsp://127.0.0.1:8554/cam3"
    "rtsp://127.0.0.1:8554/cam0"
    "rtsp://127.0.0.1:8554/cam1"
    "rtsp://127.0.0.1:8554/cam2"
)
DEFAULT_4K_YOLO="rtsp://127.0.0.1:8554/cam4"
DEFAULT_4K_TOP="rtsp://127.0.0.1:8554/cam4"
DEFAULT_4K_BOTTOM="rtsp://127.0.0.1:8554/cam5"

# YOLO engine relocation, same trick as 05: the custom YOLO parser writes
# the engine to <CWD>/model_b<N>_gpu<g>_fp<bits>.engine ignoring
# model-engine-file's directory; relocate it on the next launch so the
# cache hits.
if [[ -f "$ROOT/model_b1_gpu0_fp16.engine" && ! -f "$ROOT/models/yolo/model.engine" ]]; then
    mkdir -p "$ROOT/models/yolo"
    mv "$ROOT/model_b1_gpu0_fp16.engine" "$ROOT/models/yolo/model.engine"
    echo "[run_in_container] relocated YOLO engine to models/yolo/model.engine"
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

# If the caller provided no explicit 1080p / 4K-yolo / 4K-stitch slots, fill
# in the defaults. This mirrors how 04 and 06 handle missing URIs.
args=("$@")
has_1080p=0; has_yolo=0; has_top=0; has_bottom=0
for arg in "$@"; do
    case "$arg" in
        --uri-1080p) has_1080p=1 ;;
        --uri-4k-yolo) has_yolo=1 ;;
        --uri-4k-stitch-top) has_top=1 ;;
        --uri-4k-stitch-bottom) has_bottom=1 ;;
    esac
done
if [[ $has_1080p -eq 0 ]]; then
    for uri in "${DEFAULT_1080P[@]}"; do
        args=(--uri-1080p "$uri" "${args[@]}")
    done
fi
if [[ $has_yolo -eq 0 ]]; then
    args=(--uri-4k-yolo "$DEFAULT_4K_YOLO" "${args[@]}")
fi
if [[ $has_top -eq 0 ]]; then
    args=(--uri-4k-stitch-top "$DEFAULT_4K_TOP" "${args[@]}")
fi
if [[ $has_bottom -eq 0 ]]; then
    args=(--uri-4k-stitch-bottom "$DEFAULT_4K_BOTTOM" "${args[@]}")
fi

CONTAINER_NAME="${COMPONENT_CONTAINER_NAME:-p07-full-pressure-$(date +%Y%m%d-%H%M%S)-$$}"

cleanup_container() {
    docker stop "$CONTAINER_NAME" >/dev/null 2>&1 || true
    if [[ "$NEEDS_XHOST_CLEANUP" == "1" ]]; then
        DISPLAY="$DISPLAY_VALUE" xhost -local:root >/dev/null 2>&1 || true
    fi
}
trap cleanup_container EXIT INT TERM

# Backgrounded docker run so the wrapper can `wait` and propagate signals
# cleanly (debug 009 fix from 06). --init + non-bash entrypoint is the SIGTERM
# workaround from debug 004.
docker run --rm --init --gpus all --net=host \
    --name "$CONTAINER_NAME" \
    --entrypoint /workspace/build/full_pressure_integration \
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
