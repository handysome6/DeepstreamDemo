# Component 03 build/runtime image.
#
# Same dependency surface as components 01/02: Qt 6 dev packages on top of the
# DeepStream 9.0 multiarch image. Kept as its own image, with its own tag, so
# that 03 stays self-contained per the architecture rule (each component must
# build standalone).
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

# Sanity: latency_probe needs both videotestsrc (for the synthetic baseline)
# and nvurisrcbin (for the RTSP-mode validation).
RUN gst-inspect-1.0 videotestsrc >/dev/null \
    && gst-inspect-1.0 nvurisrcbin >/dev/null \
    && echo "videotestsrc + nvurisrcbin plugins found"
