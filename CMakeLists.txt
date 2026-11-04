cmake_minimum_required(VERSION 3.24)
project(GoodHAP LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(MSVC)
  add_compile_options(/W3 /utf-8 /MP)
  add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/O2> $<$<CONFIG:Release,RelWithDebInfo>:/fp:fast> $<$<CONFIG:Release,RelWithDebInfo>:/arch:AVX2>)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- snappy -----------------------------------------------------------------
set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SNAPPY_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/snappy EXCLUDE_FROM_ALL)

# --- Vidvox hap frame format ------------------------------------------------
add_library(hapframe STATIC third_party/hap/source/hap.c)
target_include_directories(hapframe PUBLIC third_party/hap/source)
target_link_libraries(hapframe PRIVATE snappy)
target_include_directories(hapframe PRIVATE third_party/snappy ${CMAKE_BINARY_DIR}/third_party/snappy)

# --- FFmpeg (BtbN lgpl-shared build) ----------------------------------------
set(FFMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg)
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE ${FFMPEG_DIR}/include)
target_link_libraries(ffmpeg INTERFACE
  ${FFMPEG_DIR}/lib/avformat.lib ${FFMPEG_DIR}/lib/avcodec.lib
  ${FFMPEG_DIR}/lib/avutil.lib ${FFMPEG_DIR}/lib/swscale.lib ${FFMPEG_DIR}/lib/swresample.lib)

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
target_link_libraries(ghapcore PUBLIC ffmpeg PRIVATE hapframe)

add_executable(goodhap-cli src/cli/main.cpp)
target_link_libraries(goodhap-cli PRIVATE ghapcore)

add_executable(ghap_eval tools/eval/eval.cpp)
target_link_libraries(ghap_eval PRIVATE ghapcore hapframe)
target_compile_definitions(ghap_eval PRIVATE GHAP_FFMPEG_EXE="${FFMPEG_DIR}/bin/ffmpeg.exe")

# --- GUI ----------------------------------------------------------------------
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
