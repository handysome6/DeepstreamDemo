# Component 06 build/runtime image.
#
# Same dependency surface as component 04/05, with CUDA compilation enabled
# because the stitch core is a custom CUDA kernel.
ARG BASE_IMAGE=nvcr.io/nvidia/deepstream:9.0-triton-multiarch
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        qt6-base-dev \
        qt6-base-dev-tools \
        libqt6opengl6-dev \
        libqt6openglwidgets6 \
        libegl-dev \
        libgl-dev \
        libgstrtspserver-1.0-dev \
        pkg-config \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

RUN test -d /usr/lib/x86_64-linux-gnu/cmake/Qt6 \
    && echo "Qt6 cmake config found"

# Build-time check stays limited to plugins whose .so files do not require
# libcuda/libnvidia-ml at image-build time. nvrtspoutsinkbin is verified at
# runtime when the container is launched with --gpus.
RUN gst-inspect-1.0 nvurisrcbin >/dev/null \
    && echo "nvurisrcbin plugin found at build time"
