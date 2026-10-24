# 只做头文件导出的组件：把仓库根部的 shared/ 暴露给固件。
# 协议定义只有一份，Windows 端通过相对路径引用同一个文件。
idf_component_register(
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../shared"
)
