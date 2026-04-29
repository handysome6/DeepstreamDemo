# Component 01 build/runtime image.
#
# Adds Qt 6 dev packages on top of the DeepStream 9.0 multiarch image.
# Everything else this component needs (GStreamer 1.24, EGL, NvBufSurface,
# CMake, gcc) is already in the base.
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
        pkg-config \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# Sanity: fail the image build if Qt6 is not actually discoverable.
RUN test -d /usr/lib/x86_64-linux-gnu/cmake/Qt6 \
    && echo "Qt6 cmake config found"
