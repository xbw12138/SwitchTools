cmake_minimum_required(VERSION 3.20)
project(swlink CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 项目内置的精简 ffmpeg (brew 在旧版 macOS 上装不上, 见 setup.sh)
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/third_party/ffmpeg")
set(ENV{PKG_CONFIG_PATH} "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)

# ImGui 源码拉取 (ocornut/imgui + SDL2 后端)
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
)
FetchContent_MakeAvailable(imgui)

add_executable(swlink
    src/main.cpp
    src/usb_link.cpp
    src/stream_client.cpp
    src/video_decoder.cpp
    src/audio_output.cpp
    src/file_client.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

target_include_directories(swlink PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${SDL2_INCLUDE_DIRS}
    ${LIBUSB_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)

target_link_directories(swlink PRIVATE
    ${SDL2_LIBRARY_DIRS}
    ${LIBUSB_LIBRARY_DIRS}
    ${AVCODEC_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
)

target_link_libraries(swlink PRIVATE
    ${SDL2_LIBRARIES}
    ${LIBUSB_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
)
