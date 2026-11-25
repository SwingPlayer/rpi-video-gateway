cmake_minimum_required(VERSION 3.16)

project(
    rpi_video_gateway
    VERSION 0.7.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(
    FFMPEG
    REQUIRED
    IMPORTED_TARGET
    libavcodec
    libavutil
    libavformat
    libswscale
)

add_executable(
    v4l2_capture_test

    src/main.cpp

    src/capture/v4l2_capture.cpp

    src/codec/video_decoder.cpp
    src/codec/video_encoder.cpp

    src/publish/rtsp_publisher.cpp

    src/ipc/metrics_server.cpp
)

target_include_directories(
    v4l2_capture_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    v4l2_capture_test
    PRIVATE
    PkgConfig::FFMPEG
    Threads::Threads
)

target_compile_options(
    v4l2_capture_test
    PRIVATE
    -Wall
    -Wextra
    -O2
)