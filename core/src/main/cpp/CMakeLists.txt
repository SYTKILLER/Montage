cmake_minimum_required(VERSION 3.5.0)
project(montage_core)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(compositor SHARED
        bridge/napi_module.cpp
        bridge/engine_bridge.cpp
        engine/document.cpp
        engine/engine.cpp
        io/image_import.cpp
        render/render_loop.cpp
        render/tile_renderer.cpp
        tiles/pixel_probe.cpp)

target_include_directories(compositor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(compositor PUBLIC
        libace_napi.z.so
        libhilog_ndk.z.so
        libnative_window.so
        libnative_vsync.so
        libpixelmap.so
        libimage_source.so
        libEGL.so
        libGLESv3.so)
