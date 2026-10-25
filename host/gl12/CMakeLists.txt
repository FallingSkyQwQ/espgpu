# opengl32.dll — GL 1.2 固定管线前端（M3 / M4）
#
# 导出层（gen/）由 tools/gen_gl.py 生成并签入；改了 impl_* 的实现集合后重跑脚本即可。
# 用法：把产出的 opengl32.dll 与 espgpu.dll 放在目标程序（如 dsda-doom.exe）旁边。

add_library(espgpu_gl12 SHARED
    gen/gl_exports.cpp
    gen/gl_stubs.cpp
    gl_trace.cpp
    gl_state.cpp
    gl_matrix.cpp
    gl_vertex.cpp
    gl_draw.cpp
    gl_texture.cpp
    gl_list.cpp
    gl_misc.cpp
    gl_device.cpp
    gl_wgl.cpp
    opengl32.def)

set_target_properties(espgpu_gl12 PROPERTIES
    OUTPUT_NAME opengl32
    PREFIX ""
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)

target_include_directories(espgpu_gl12 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(espgpu_gl12 PRIVATE espgpu gdi32 user32 winmm)

if(MSVC)
    # 生成的导出文件很大：开 /bigobj；GL 入口大量未用参数
    target_compile_options(espgpu_gl12 PRIVATE /bigobj /wd4100)
endif()

add_custom_command(TARGET espgpu_gl12 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:espgpu> $<TARGET_FILE_DIR:espgpu_gl12>)
