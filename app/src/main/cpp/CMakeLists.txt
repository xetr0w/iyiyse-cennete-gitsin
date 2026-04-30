cmake_minimum_required(VERSION 3.22.1)

project("notia_drawing_engine")

# TODO: Skia kütüphane yollarını ortamınıza göre ayarlayın
set(SKIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(
        drawing_engine
        SHARED
        drawing_engine_jni.cpp
        DrawingEngine.cpp
        tools/BallpointRenderer.cpp
        tools/HighlighterRenderer.cpp
        tools/EraserRenderer.cpp
)

target_include_directories(drawing_engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SKIA_DIR}
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)

# Not: Skia statik/dinamik kütüphanesini projeye dahil ettiğinizde buraya ekleyin
# target_link_libraries(drawing_engine PRIVATE skia)

target_link_libraries(
        drawing_engine
        ${log-lib}
        ${android-lib}
        ${egl-lib}
        ${gles-lib}
)
