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

# ── espgpu_core — 设备核心（与固件同一份 C 代码），供软件模拟器使用 ──
set(ESPGPU_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/core")
add_library(espgpu_core STATIC
    ${ESPGPU_CORE_DIR}/core.c
    ${ESPGPU_CORE_DIR}/tnl.c
    ${ESPGPU_CORE_DIR}/raster3d.c
    ${ESPGPU_CORE_DIR}/tex.c
    ${ESPGPU_CORE_DIR}/vm.c
    ${ESPGPU_CORE_DIR}/raster2d.c
    ${ESPGPU_CORE_DIR}/port_host.c)
target_include_directories(espgpu_core PUBLIC ${ESPGPU_CORE_DIR} ${ESPGPU_SHARED_DIR})
set_target_properties(espgpu_core PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

# ── espgpu.dll — 设备接口层 ─────────────────────────────────────────
add_library(espgpu SHARED
    src/api.cpp
    src/link.cpp
    src/winusb.cpp
    src/emulator.cpp
    src/decode.cpp
    src/draw.cpp
    src/draw3d.cpp
    src/panel_api.cpp
    src/log.cpp)
target_include_directories(espgpu
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include ${ESPGPU_SHARED_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(espgpu PRIVATE ESPGPU_BUILD_DLL)
# MinGW 默认会加 lib 前缀；前端和文档都按 espgpu.dll 找它
set_target_properties(espgpu PROPERTIES PREFIX "")
target_link_libraries(espgpu PRIVATE espgpu_core winusb setupapi gdi32)

# ── espgpu-panel.exe — 控制面板 / 帧查看器（Material 3 风格） ───────
add_subdirectory(panel)

# ── espgpu-bench.exe — 带宽 / 压缩率实测 ────────────────────────────
add_executable(espgpu-bench
    tests/bench.cpp)
target_link_libraries(espgpu-bench PRIVATE espgpu)

# ── espgpu-core-test.exe — 设备核心的渲染冒烟测试（不需要硬件） ────────
add_executable(espgpu-core-test tests/core_test.c)
target_link_libraries(espgpu-core-test PRIVATE espgpu_core)
if(NOT MSVC)
    target_link_libraries(espgpu-core-test PRIVATE m)
endif()
enable_testing()
add_test(NAME core COMMAND espgpu-core-test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# ── espgpu-demo.exe — M2 的 2D 渲染演示 ─────────────────────────────
add_executable(espgpu-demo
    demo/main.cpp)
target_link_libraries(espgpu-demo PRIVATE espgpu)

# ── espgpu-demo3d.exe — M5 的 3D 演示（旋转立方体 + 地形 + 光照） ──
add_executable(espgpu-demo3d
    demo3d/main.cpp)
target_link_libraries(espgpu-demo3d PRIVATE espgpu winmm)

target_link_libraries(espgpu-demo PRIVATE winmm)

# ── 前端 ─────────────────────────────────────────────────────────────
add_subdirectory(gl12)    # opengl32.dll（GL 1.2 固定管线 + trace / 直通）
add_subdirectory(glsl)    # GLSL ES 3.0 编译器（静态库）
add_subdirectory(gles3)   # libGLESv2.dll + libEGL.dll + GLES demo

# 让 exe 能从输出目录直接找到 DLL
add_custom_command(TARGET espgpu-bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:espgpu> $<TARGET_FILE_DIR:espgpu-bench>)
add_custom_command(TARGET espgpu-demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:espgpu> $<TARGET_FILE_DIR:espgpu-demo>)
add_custom_command(TARGET espgpu-demo3d POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:espgpu> $<TARGET_FILE_DIR:espgpu-demo3d>)
