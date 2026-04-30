#!/usr/bin/env bash
# Stages model assets for component 05.
#
# - TrafficCamNet (the zero-friction default): nothing to do, ships in the
#   DeepStream image. The default nvinfer config points directly at
#   /opt/nvidia/deepstream/deepstream-9.0/samples/models/Primary_Detector.
#
# - YOLOv8: this script only fetches the .pt checkpoint and the COCO labels.
#   ONNX export still requires ultralytics + onnx, which we deliberately do
#   NOT install into the runtime image. Run the export step on a host that has
#   ultralytics, e.g. `yolo export model=yolov8n.pt format=onnx opset=12 imgsz=640`,
#   then drop the resulting yolov8n.onnx alongside this directory under
#   ./models/, plus the matching parser library.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
MODELS_DIR="$ROOT/models"
CONFIGS_DIR="$ROOT/configs"
mkdir -p "$MODELS_DIR" "$CONFIGS_DIR"

YOLO_PT_URL="${YOLO_PT_URL:-https://github.com/ultralytics/assets/releases/download/v8.3.0/yolov8n.pt}"
COCO_LABELS_URL="${COCO_LABELS_URL:-https://raw.githubusercontent.com/AlexeyAB/darknet/master/data/coco.names}"

if [[ "${SKIP_YOLO:-0}" == "1" ]]; then
    echo "[fetch_models] SKIP_YOLO=1, skipping YOLO checkpoint fetch."
    exit 0
fi

if [[ ! -f "$MODELS_DIR/yolov8n.pt" ]]; then
    echo "[fetch_models] downloading $YOLO_PT_URL -> models/yolov8n.pt"
    curl -fL "$YOLO_PT_URL" -o "$MODELS_DIR/yolov8n.pt"
else
    echo "[fetch_models] models/yolov8n.pt already present"
fi

if [[ ! -f "$CONFIGS_DIR/labels_coco80.txt" ]]; then
    echo "[fetch_models] downloading $COCO_LABELS_URL -> configs/labels_coco80.txt"
    curl -fL "$COCO_LABELS_URL" -o "$CONFIGS_DIR/labels_coco80.txt"
else
    echo "[fetch_models] configs/labels_coco80.txt already present"
fi

cat <<EOF
[fetch_models] checkpoint staged at models/yolov8n.pt
[fetch_models] next steps for the YOLO path are MANUAL — see configs/config_infer_primary_yolov8.txt:
  - export ONNX (ultralytics): yolo export model=models/yolov8n.pt format=onnx opset=12 imgsz=640
  - build a parser .so (marcoslucianops/DeepStream-Yolo or similar) into models/libnvdsinfer_custom_impl_Yolo.so
  - drop the resulting .onnx and .so under models/ then point --infer-config at configs/config_infer_primary_yolov8.txt
EOF
