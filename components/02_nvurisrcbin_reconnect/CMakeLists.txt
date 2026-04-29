cmake_minimum_required(VERSION 3.18)
project(nvurisrcbin_reconnect LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ─── Qt 6 ─────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

# ─── GStreamer via pkg-config ──────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0)

# ─── CUDA toolkit for CUDA-GL interop ──────────────────────────────────────
find_package(CUDAToolkit REQUIRED)

# ─── DeepStream / NvBufSurface ────────────────────────────────────────────
# Override with -DDEEPSTREAM_DIR=/your/path if installed elsewhere.
set(DEEPSTREAM_DIR "/opt/nvidia/deepstream/deepstream"
    CACHE PATH "DeepStream SDK install path")

find_path(NVBUFSURFACE_INCLUDE_DIR
    NAMES nvbufsurface.h
    HINTS "${DEEPSTREAM_DIR}/sources/includes"
    NO_DEFAULT_PATH)
find_library(NVBUFSURFACE_LIB
    NAMES nvbufsurface
    HINTS "${DEEPSTREAM_DIR}/lib"
    NO_DEFAULT_PATH)

if(NOT NVBUFSURFACE_INCLUDE_DIR OR NOT NVBUFSURFACE_LIB)
    message(FATAL_ERROR
        "NvBufSurface not found under ${DEEPSTREAM_DIR}. "
        "Pass -DDEEPSTREAM_DIR=/path/to/deepstream to point at your install.")
endif()

# ─── Target ───────────────────────────────────────────────────────────────
add_executable(nvurisrcbin_reconnect
    src/main.cpp
    src/VideoGLWidget.cpp
    src/RtspSource.cpp)

target_include_directories(nvurisrcbin_reconnect PRIVATE
    src
    ${NVBUFSURFACE_INCLUDE_DIR}
    ${CUDAToolkit_INCLUDE_DIRS})

target_link_libraries(nvurisrcbin_reconnect PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    PkgConfig::GST
    CUDA::cudart
    ${NVBUFSURFACE_LIB})

set_target_properties(nvurisrcbin_reconnect PROPERTIES
    INSTALL_RPATH "${DEEPSTREAM_DIR}/lib"
    BUILD_RPATH   "${DEEPSTREAM_DIR}/lib")
