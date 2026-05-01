#!/usr/bin/env bash
# Stages YOLOv10s assets for component 07 into models/yolo/.
#
# Same pattern as 05/scripts/fetch_models.sh: copy a known-good v10s ONNX +
# custom bbox parser .so from a sibling project. Override YOLO_SRC_DIR if your
# YOLO assets live elsewhere. The destination layout under models/yolo/ must
# match the paths in configs/config_infer_primary_yolov10.txt.
#
# 07 does not stage the TrafficCamNet smoke path because the YOLO panel is
# always-on under full pressure; if you want a TrafficCamNet smoke, use 05.
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

Set YOLO_SRC_DIR to a directory that contains:
    yolov10s.onnx
    yolov10s.onnx.data
    libnvdsinfer_custom_impl_Yolo.so
    labels.txt

Or copy them from components/05_selective_yolo_infer/models/yolo/ if you
have already staged them there.
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
[fetch_models] Now run:
  ./scripts/run_in_container.sh --stage full
EOF
