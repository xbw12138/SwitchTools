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

# ImGui 源码内置在 third_party/imgui(ocornut/imgui v1.91.5),构建时无需联网
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "缺少 ${IMGUI_DIR}/imgui.cpp,请放入 ocornut/imgui v1.91.5 源码")
endif()

add_executable(swlink
    src/main.cpp
    src/usb_link.cpp
    src/stream_client.cpp
    src/video_decoder.cpp
    src/audio_output.cpp
    src/file_client.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

target_include_directories(swlink PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
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

# ---- 命令行工具:通过 USB 操作 SD 卡(含 NRO 推送) ----
add_executable(swlink-cli
    src/cli.cpp
    src/usb_link.cpp
    src/file_client.cpp
)

target_include_directories(swlink-cli PRIVATE ${LIBUSB_INCLUDE_DIRS})
target_link_directories(swlink-cli PRIVATE ${LIBUSB_LIBRARY_DIRS})
target_link_libraries(swlink-cli PRIVATE ${LIBUSB_LIBRARIES})

# ---- 打包成可直接双击的 SwLink.app ----
set_target_properties(swlink PROPERTIES
    OUTPUT_NAME "SwLink"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/macos/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "SwLink"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.2.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.2.0"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.swlink.client"
)

# 链接后自动把 SDK 依赖拷进 App、加图标并签名
add_custom_command(TARGET swlink POST_BUILD
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/bundle_app.sh"
            "$<TARGET_BUNDLE_DIR:swlink>" "$<TARGET_FILE:swlink>"
            "${CMAKE_SOURCE_DIR}/macos/AppIcon.icns"
    COMMENT "打包 SwLink.app(内置依赖库 + 图标)"
    VERBATIM
)
