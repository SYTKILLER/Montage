cmake_minimum_required(VERSION 3.5.0)
project(montage_core)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(compositor SHARED
        bridge/napi_module.cpp
        bridge/engine_bridge.cpp
        engine/brush.cpp
        engine/clipboard_ops.cpp
        engine/document.cpp
        engine/engine.cpp
        engine/heal_ops.cpp
        engine/content_aware_ops.cpp
        engine/poisson_ops.cpp
        engine/inpaint_ops.cpp
        engine/seam_ops.cpp
        engine/gradient_ops.cpp
        engine/group_ops.cpp
        engine/layer_ops.cpp
        engine/style_ops.cpp
        engine/history.cpp
        engine/shape_ops.cpp
        engine/vector_path.cpp
        engine/vector_flatten.cpp
        engine/vector_shapes.cpp
        engine/vector_raster.cpp
        engine/vector_stroke.cpp
        engine/vector_edit.cpp
        engine/vector_render.cpp
        engine/filter_ops.cpp
        engine/filters.cpp
        engine/text_ops.cpp
        engine/text_raster_oh.cpp
        engine/transform_ops.cpp
        engine/resample.cpp
        engine/view_ops.cpp
        io/image_import.cpp
        io/image_export.cpp
        io/psd_export.cpp
        io/png_decode.cpp
        io/dng_decode.cpp
        io/project_open.cpp
        io/project_save.cpp
        io/psd/psd_reader.cpp
        io/psd/psd_import.cpp
        render/render_loop.cpp
        render/tile_renderer.cpp
        tiles/pixel_probe.cpp
        tiles/selection_outline.cpp
        tiles/selection_ops.cpp
        tiles/quick_mask.cpp
        tiles/shape_mask.cpp
        tiles/magic_wand.cpp)

target_include_directories(compositor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(compositor PUBLIC
        libace_napi.z.so
        libhilog_ndk.z.so
        libnative_window.so
        libnative_vsync.so
        libpixelmap.so
        libimage_source.so
        libEGL.so
        libGLESv3.so
        libnative_drawing.so
        z)
