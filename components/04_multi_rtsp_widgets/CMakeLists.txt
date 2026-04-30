cmake_minimum_required(VERSION 3.18)
project(multi_rtsp_widgets LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

add_executable(multi_rtsp_widgets
    src/main.cpp
    src/VideoGLWidget.cpp
    src/RtspSource.cpp)

target_include_directories(multi_rtsp_widgets PRIVATE
    src
    ${NVBUFSURFACE_INCLUDE_DIR}
    ${CUDAToolkit_INCLUDE_DIRS})

target_link_libraries(multi_rtsp_widgets PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    PkgConfig::GST
    CUDA::cudart
    ${NVBUFSURFACE_LIB})

set_target_properties(multi_rtsp_widgets PROPERTIES
    INSTALL_RPATH "${DEEPSTREAM_DIR}/lib"
    BUILD_RPATH   "${DEEPSTREAM_DIR}/lib")
