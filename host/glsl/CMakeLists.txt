if(NOT ESPGPU_SHARED_DIR)
    set(ESPGPU_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../shared")
endif()

# espgpu_glsl — GLSL ES 3.00 / 1.00 编译器（纯 C++，平台无关）
add_library(espgpu_glsl STATIC
    src/preprocess.cpp
    src/parser.cpp
    src/lower.cpp
    src/codegen.cpp
    src/dag.cpp
    src/compiler.cpp)
target_include_directories(espgpu_glsl
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include ${ESPGPU_SHARED_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(espgpu_glsl PUBLIC cxx_std_20)
set_target_properties(espgpu_glsl PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(espgpu_glsl PRIVATE /W4 /wd4244 /wd4267)
endif()

# 单元测试：任何平台都能跑（不依赖设备 / Windows）
add_executable(glsl-tests tests/glsl_tests.cpp)
target_link_libraries(glsl-tests PRIVATE espgpu_glsl)
target_include_directories(glsl-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
