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

# Stage the SDK-shipped TrafficCamNet model into the bind-mounted models/
# directory so the TRT-compiled engine survives across container instances.
# nvinfer writes the engine alongside the ONNX file regardless of
# model-engine-file, so the ONNX itself has to live under /workspace.
mkdir -p "$ROOT/models"
if [[ ! -f "$ROOT/models/resnet18_trafficcamnet_pruned.onnx" ]]; then
    echo "[run_in_container] staging TrafficCamNet ONNX into models/ via the build image"
    docker run --rm --entrypoint bash \
        -v "$ROOT:/workspace" \
        "$IMAGE" \
        -c 'cp -n /opt/nvidia/deepstream/deepstream-9.0/samples/models/Primary_Detector/resnet18_trafficcamnet_pruned.onnx /workspace/models/ && cp -n /opt/nvidia/deepstream/deepstream-9.0/samples/models/Primary_Detector/labels.txt /workspace/models/ && cp -n /opt/nvidia/deepstream/deepstream-9.0/samples/models/Primary_Detector/cal_trt.bin /workspace/models/ && chown -R '"$(id -u):$(id -g)"' /workspace/models'
fi

# YOLOv10 engine quirk: the custom NvDsInferYoloCudaEngineGet function in
# libnvdsinfer_custom_impl_Yolo.so writes the engine to <CWD>/model_b<N>_gpu<g>_fp<bits>.engine,
# ignoring model-engine-file's directory. So if a fresh build dropped the
# engine at /workspace/model_b1_gpu0_fp16.engine, relocate it under
# models/yolo/ where the YOLO config expects it.
if [[ -f "$ROOT/model_b1_gpu0_fp16.engine" && ! -f "$ROOT/models/yolo/model.engine" ]]; then
    mkdir -p "$ROOT/models/yolo"
    mv "$ROOT/model_b1_gpu0_fp16.engine" "$ROOT/models/yolo/model.engine"
    echo "[run_in_container] relocated YOLO engine to models/yolo/model.engine"
fi

if command -v xhost >/dev/null 2>&1; then
    xhost +local:root >/dev/null
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

# --init             : install tini as PID 1 so SIGTERM from `timeout` /
#                       Ctrl-C reliably reaches the Qt binary.
# --entrypoint <bin> : skip the DeepStream image's bash entrypoint.sh; that
#                       shell does not exec its child, so SIGTERM never reaches
#                       the binary and orphan containers pile up after `timeout`.
# --name             : stable handle for `docker stop` cleanup if the host
#                       script is interrupted before the container exits.
CONTAINER_NAME="${COMPONENT_CONTAINER_NAME:-p05-selective-yolo-$(date +%Y%m%d-%H%M%S)-$$}"

cleanup_container() {
    docker stop "$CONTAINER_NAME" >/dev/null 2>&1 || true
}
trap 'cleanup_container; xhost -local:root >/dev/null 2>&1 || true' EXIT INT TERM

docker run --rm --init --gpus all --net=host \
    --name "$CONTAINER_NAME" \
    --entrypoint /workspace/build/selective_yolo_infer \
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
    "${args[@]}"
