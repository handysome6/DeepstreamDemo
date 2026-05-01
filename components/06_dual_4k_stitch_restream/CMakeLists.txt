cmake_minimum_required(VERSION 3.19)
project(dual_4k_stitch_restream LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_ARCHITECTURES 120-real)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
    gstreamer-rtsp-server-1.0)

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

add_executable(dual_4k_stitch_restream
    src/main.cpp
    src/VideoGLWidget.cpp
    src/RtspSource.cpp
    src/DualRtspSourcePair.cpp
    src/StitchRenderer.cpp
    src/StitchRestreamSession.cpp
    src/StitchCuda.cu)

set_target_properties(dual_4k_stitch_restream PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    INSTALL_RPATH "${DEEPSTREAM_DIR}/lib"
    BUILD_RPATH   "${DEEPSTREAM_DIR}/lib")

target_include_directories(dual_4k_stitch_restream PRIVATE
    src
    ${NVBUFSURFACE_INCLUDE_DIR}
    ${CUDAToolkit_INCLUDE_DIRS})

target_link_libraries(dual_4k_stitch_restream PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    PkgConfig::GST
    CUDA::cudart
    ${NVBUFSURFACE_LIB})
