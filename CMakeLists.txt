cmake_minimum_required(VERSION 3.24)
project(GoodHAP LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W3 /utf-8 /MP)
  add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/O2> $<$<CONFIG:Release,RelWithDebInfo>:/fp:fast> $<$<CONFIG:Release,RelWithDebInfo>:/arch:AVX2>)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:-O3>)
endif()

# --- snappy -----------------------------------------------------------------
set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SNAPPY_INSTALL OFF CACHE BOOL "" FORCE)
# snappy 1.2.1 asks for CMake 3.1, which CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(third_party/snappy EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# --- Vidvox hap frame format ------------------------------------------------
add_library(hapframe STATIC third_party/hap/source/hap.c)
target_include_directories(hapframe PUBLIC third_party/hap/source)
target_link_libraries(hapframe PRIVATE snappy)
target_include_directories(hapframe PRIVATE third_party/snappy ${CMAKE_BINARY_DIR}/third_party/snappy)

# --- FFmpeg -------------------------------------------------------------------
# Windows: BtbN lgpl-shared build in third_party/ffmpeg (or GHAP_FFMPEG_DIR).
# macOS/Linux: system FFmpeg 5 or newer through pkg-config (brew install ffmpeg).
add_library(ffmpeg INTERFACE)
if(WIN32)
  set(GHAP_FFMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg CACHE PATH "FFmpeg shared build (include, lib, bin)")
  set(FFMPEG_DIR ${GHAP_FFMPEG_DIR})
  target_include_directories(ffmpeg INTERFACE ${FFMPEG_DIR}/include)
  target_link_libraries(ffmpeg INTERFACE
    ${FFMPEG_DIR}/lib/avformat.lib ${FFMPEG_DIR}/lib/avcodec.lib
    ${FFMPEG_DIR}/lib/avutil.lib ${FFMPEG_DIR}/lib/swscale.lib ${FFMPEG_DIR}/lib/swresample.lib)
  set(FFMPEG_EXE ${FFMPEG_DIR}/bin/ffmpeg.exe)
else()
  find_package(PkgConfig REQUIRED)
  # libavformat 59 = FFmpeg 5, the first with the const AVInputFormat/AVCodec API.
  pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat>=59 libavcodec libavutil libswscale)
  if(NOT FFMPEG_FOUND)
    message(FATAL_ERROR "FFmpeg 5 or newer not found through pkg-config. On a Mac: brew install ffmpeg (or brew upgrade ffmpeg), then run build-mac.sh again.")
  endif()
  target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
  set(FFMPEG_EXE ffmpeg)
endif()

# --- core -------------------------------------------------------------------
add_library(ghapcore STATIC
  src/core/block_encode.cpp
  src/core/block_decode.cpp
  src/core/bluenoise.cpp
  src/core/frame_encoder.cpp
  src/core/media_reader.cpp
  src/core/mov_writer.cpp
  src/core/transcode.cpp
  src/core/metrics.cpp
)
target_include_directories(ghapcore PUBLIC src/core)
find_package(Threads REQUIRED)
target_link_libraries(ghapcore PUBLIC ffmpeg Threads::Threads PRIVATE hapframe)

add_executable(goodhap-cli src/cli/main.cpp)
target_link_libraries(goodhap-cli PRIVATE ghapcore)

add_executable(ghap_eval tools/eval/eval.cpp)
target_link_libraries(ghap_eval PRIVATE ghapcore hapframe)
target_compile_definitions(ghap_eval PRIVATE GHAP_FFMPEG_EXE="${FFMPEG_EXE}")

# --- GUI (Windows only) ---------------------------------------------------------
if(WIN32)
  set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
  add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
  target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

  add_executable(GoodHAP WIN32 src/app/main.cpp src/app/app.cpp src/app/preview_renderer.cpp)
  target_link_libraries(GoodHAP PRIVATE ghapcore imgui d3d11 d3dcompiler dxgi dwmapi shcore)
  target_compile_definitions(GoodHAP PRIVATE UNICODE _UNICODE)

  # copy FFmpeg dlls next to the executables
  file(GLOB FFMPEG_DLLS ${FFMPEG_DIR}/bin/*.dll)
  add_custom_target(copy_ffmpeg_dlls ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin/$<CONFIG>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
endif()
