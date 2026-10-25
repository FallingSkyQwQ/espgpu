# espgpu-panel.exe — 控制面板 / 帧查看器（Material 3 风格）
#
# 依赖顶层 CMakeLists.txt 定义的 imgui 与 espgpu 目标。

add_executable(espgpu-panel WIN32
    app.cpp
    theme.cpp
    widgets.cpp
    viewer.cpp
    pages/overview.cpp
    pages/display.cpp
    pages/performance.cpp
    pages/viewer_page.cpp
    pages/debug.cpp
    pages/memory.cpp
    pages/clock.cpp
    pages/firmware.cpp
    pages/log.cpp
    pages/appearance.cpp)

target_include_directories(espgpu-panel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(espgpu-panel PRIVATE
    espgpu imgui d3d11 dxgi comdlg32 dwmapi advapi32 user32 gdi32)
target_compile_definitions(espgpu-panel PRIVATE
    ESPGPU_SHARED_DIR="${ESPGPU_SHARED_DIR}" UNICODE _UNICODE)

if(MSVC)
    # 源码里有中文注释与 UTF-8 字面量
    target_compile_options(espgpu-panel PRIVATE /utf-8)
endif()

# MinGW 没有 wWinMain 的默认入口
if(MINGW)
    target_link_options(espgpu-panel PRIVATE -municode)
    # MSVC 通过 imgui 后端里的 #pragma comment 链接这些，MinGW 需要显式写出
    target_link_libraries(espgpu-panel PRIVATE d3dcompiler_47 imm32)
endif()

add_custom_command(TARGET espgpu-panel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:espgpu> $<TARGET_FILE_DIR:espgpu-panel>)
