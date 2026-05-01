cmake_minimum_required(VERSION 3.19)
project(full_pressure_integration LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
# Mandatory for the StitchCuda kernel on RTX 5090 / Blackwell. See
# docs/debug/008 - p06-calibrated-kernel-needs-sm120-real.md.
set(CMAKE_CUDA_ARCHITECTURES 120-real)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0)

find_package(CUDAToolkit REQUIRED)

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

add_executable(full_pressure_integration
    src/main.cpp
    src/VideoGLWidget.cpp
    src/StitchGLWidget.cpp
    src/RtspSource.cpp
    src/RtspInferSource.cpp
    src/DualRtspSourcePair.cpp
    src/StitchRenderer.cpp
    src/StitchCuda.cu)
# StitchFrame.cpp is intentionally NOT compiled — its sole function
# (cloneStitchedFrame) is also defined in StitchCuda.cu and that copy is the
# linked one. The .cpp file is dead code carried forward from 06's tree to
# keep the source surface byte-equal; including it here would produce a
# duplicate-symbol link error and, on this image, a cuda_runtime.h pragma
# rejection because the .cpp's include order conflicts with newer CUDA 13.1
# headers.

set_target_properties(full_pressure_integration PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    INSTALL_RPATH "${DEEPSTREAM_DIR}/lib"
    BUILD_RPATH   "${DEEPSTREAM_DIR}/lib")

target_include_directories(full_pressure_integration PRIVATE
    src
    ${NVBUFSURFACE_INCLUDE_DIR}
    ${CUDAToolkit_INCLUDE_DIRS})

target_link_libraries(full_pressure_integration PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    PkgConfig::GST
    CUDA::cudart
    ${NVBUFSURFACE_LIB})
