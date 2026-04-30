#!/usr/bin/env bash
# Stages model assets for component 05 into models/.
#
# Two paths are supported:
#
# 1. TrafficCamNet (the zero-friction default)
#    Nothing to do here — scripts/run_in_container.sh stages the SDK-shipped
#    ONNX/labels/cal_trt.bin into /workspace/models on first invocation.
#
# 2. YOLOv10s (the production target for "selective YOLO inference")
#    A working v10s ONNX + custom bbox parser .so already lives in the
#    sibling project at ../../DeepstreamPOC/models/yolo/. We copy it in
#    instead of re-fetching/exporting from scratch.
#
# Override YOLO_SRC_DIR if your YOLO assets live elsewhere. The destination
# layout under models/yolo/ must match the paths in
# configs/config_infer_primary_yolov10.txt.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
MODELS_DIR="$ROOT/models"
YOLO_DIR="$MODELS_DIR/yolo"
mkdir -p "$YOLO_DIR"

YOLO_SRC_DIR="${YOLO_SRC_DIR:-$ROOT/../../../DeepstreamPOC/models/yolo}"

if [[ "${SKIP_YOLO:-0}" == "1" ]]; then
    echo "[fetch_models] SKIP_YOLO=1 — skipping YOLO asset staging."
    exit 0
fi

if [[ ! -d "$YOLO_SRC_DIR" ]]; then
    cat >&2 <<EOF
[fetch_models] YOLO source directory not found: $YOLO_SRC_DIR

If you have the assets elsewhere, set YOLO_SRC_DIR to a directory that
contains:
    yolov10s.onnx
    yolov10s.onnx.data
    libnvdsinfer_custom_impl_Yolo.so
    labels.txt

Otherwise, set SKIP_YOLO=1 to leave the YOLO config unwired and run with the
default --infer-config configs/config_infer_primary_trafficcamnet.txt.
EOF
    exit 1
fi

REQUIRED=(yolov10s.onnx yolov10s.onnx.data libnvdsinfer_custom_impl_Yolo.so labels.txt)
missing=0
for f in "${REQUIRED[@]}"; do
    if [[ ! -f "$YOLO_SRC_DIR/$f" ]]; then
        echo "[fetch_models] missing in source: $YOLO_SRC_DIR/$f" >&2
        missing=1
    fi
done
[[ $missing -eq 0 ]] || exit 1

for f in "${REQUIRED[@]}"; do
    if [[ ! -f "$YOLO_DIR/$f" ]] || [[ "$YOLO_SRC_DIR/$f" -nt "$YOLO_DIR/$f" ]]; then
        cp -v "$YOLO_SRC_DIR/$f" "$YOLO_DIR/$f"
    fi
done

cat <<EOF
[fetch_models] YOLOv10s assets staged under models/yolo/.
[fetch_models] To run the component with this YOLO config:
  ./scripts/run_in_container.sh --infer-config configs/config_infer_primary_yolov10.txt --infer 1
EOF
