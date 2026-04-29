#!/usr/bin/env bash
# Build the prototype inside the Qt6+DeepStream container.
#
# First run will build the image (a few minutes for the apt-install layer);
# subsequent runs just compile.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

IMAGE="${COMPONENT_IMAGE:-deepstream-demo/01-qt-eglimage:latest}"

# Build (or re-use) the component image.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "[build_in_container] building image $IMAGE"
    docker build -t "$IMAGE" -f "$ROOT/docker/Dockerfile" "$ROOT/docker"
fi

# Compile.
docker run --rm --gpus all --entrypoint bash \
    -v "$ROOT:/workspace" \
    -w /workspace \
    "$IMAGE" \
    -c 'cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"'
