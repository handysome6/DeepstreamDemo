# Component 05 build/runtime image.
#
# Same dependency surface as component 04, kept as its own image so this
# component stays self-contained per the architecture rule. nvinfer + nvdsosd
# already ship inside the base DeepStream image.
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

RUN test -d /usr/lib/x86_64-linux-gnu/cmake/Qt6 \
    && echo "Qt6 cmake config found"

RUN gst-inspect-1.0 nvurisrcbin >/dev/null \
    && gst-inspect-1.0 nvinfer    >/dev/null \
    && gst-inspect-1.0 nvdsosd    >/dev/null \
    && gst-inspect-1.0 nvstreammux >/dev/null \
    && echo "nvurisrcbin / nvinfer / nvdsosd / nvstreammux plugins found"
