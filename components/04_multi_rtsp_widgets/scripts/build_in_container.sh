#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

IMAGE="${COMPONENT_IMAGE:-deepstream-demo/04-multi-rtsp-widgets:latest}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "[build_in_container] building image $IMAGE"
    docker build -t "$IMAGE" -f "$ROOT/docker/Dockerfile" "$ROOT/docker"
fi

docker run --rm --gpus all --entrypoint bash \
    -e GST_PLUGIN_PATH="/opt/nvidia/deepstream/deepstream-9.0/lib/gst-plugins:${GST_PLUGIN_PATH:-}" \
    -e LD_LIBRARY_PATH="/opt/nvidia/deepstream/deepstream-9.0/lib:${LD_LIBRARY_PATH:-}" \
    -v "$ROOT:/workspace" \
    -w /workspace \
    "$IMAGE" \
    -c 'cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"'
