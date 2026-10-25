# GLES 3.0 前端（M6）：libGLESv2.dll + libEGL.dll + 两个验收 demo
#
# 程序按名字链接 libGLESv2 / libEGL，可以用 Khronos 官方头文件编译，
# 也可以用 include/ 下与官方枚举值一致的精简头文件。

set(ESPGPU_GLES_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(espgpu_gles3 SHARED
    src/gl_objects.cpp
    src/gl_draw.cpp
    src/bridge.cpp
    libGLESv2.def)
set_target_properties(espgpu_gles3 PROPERTIES OUTPUT_NAME libGLESv2 PREFIX "")
target_include_directories(espgpu_gles3 PRIVATE ${ESPGPU_GLES_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(espgpu_gles3 PRIVATE ESPGPU_GLES_BUILD)
target_link_libraries(espgpu_gles3 PRIVATE espgpu espgpu_glsl user32 gdi32)

add_library(espgpu_egl SHARED
    src/egl.cpp
    libEGL.def)
set_target_properties(espgpu_egl PROPERTIES OUTPUT_NAME libEGL PREFIX "")
target_include_directories(espgpu_egl PRIVATE ${ESPGPU_GLES_INCLUDE})
target_link_libraries(espgpu_egl PRIVATE espgpu_gles3 user32)

# ── demo ────────────────────────────────────────────────────────────
foreach(demo triangle shader)
    add_executable(gles-${demo} WIN32 demo/${demo}.cpp demo/window.cpp)
    target_include_directories(gles-${demo} PRIVATE ${ESPGPU_GLES_INCLUDE})
    target_link_libraries(gles-${demo} PRIVATE espgpu_egl espgpu_gles3 user32 winmm)
    if(MINGW)
        target_link_options(gles-${demo} PRIVATE -municode) # 入口是 wWinMain
    endif()
    add_custom_command(TARGET gles-${demo} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:espgpu> $<TARGET_FILE:espgpu_gles3> $<TARGET_FILE:espgpu_egl>
                $<TARGET_FILE_DIR:gles-${demo}>)
endforeach()
