cmake_minimum_required(VERSION 3.21)
project(espgpu_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

set(ESPGPU_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shared")

# ── 依赖：Dear ImGui ────────────────────────────────────────────────
# 允许用 -DIMGUI_DIR=<path> 指定本地副本，避免离线环境拉不下来。
if(IMGUI_DIR)
    set(imgui_SOURCE_DIR "${IMGUI_DIR}")
    message(STATUS "Using local ImGui at ${IMGUI_DIR}")
else()
    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.9
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(imgui)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC d3d11 dxgi)
if(MSVC)
    target_compile_options(imgui PRIVATE /W3 /wd4996)
endif()

# ── espgpu.dll — 设备接口层 ─────────────────────────────────────────
add_library(espgpu SHARED
    src/api.cpp
    src/link.cpp
    src/winusb.cpp
    src/decode.cpp
    src/log.cpp)
target_include_directories(espgpu
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include ${ESPGPU_SHARED_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(espgpu PRIVATE ESPGPU_BUILD_DLL)
target_link_libraries(espgpu PRIVATE winusb setupapi)

# ── espgpu-panel.exe — 控制面板 / 帧查看器 ──────────────────────────
add_executable(espgpu-panel WIN32
    panel/main.cpp)
target_link_libraries(espgpu-panel PRIVATE espgpu imgui d3d11 dxgi)
target_compile_definitions(espgpu-panel PRIVATE ESPGPU_SHARED_DIR="${ESPGPU_SHARED_DIR}")

# ── espgpu-bench.exe — 带宽 / 压缩率实测 ────────────────────────────
add_executable(espgpu-bench
    tests/bench.cpp)
target_link_libraries(espgpu-bench PRIVATE espgpu)

# 让 exe 能从输出目录直接找到 DLL
add_custom_command(TARGET espgpu-panel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:espgpu> $<TARGET_FILE_DIR:espgpu-panel>)
add_custom_command(TARGET espgpu-bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:espgpu> $<TARGET_FILE_DIR:espgpu-bench>)
