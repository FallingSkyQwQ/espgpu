# 设备核心（firmware/core）是可移植 C，主机端软件模拟器编译同一份源码。
set(ESPGPU_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../core")

idf_component_register(
    SRCS
        "app_main.c"
        "usb_descriptors.c"
        "usb_device.c"
        "framebuffer.c"
        "gpu.c"
        "port_esp.c"
        "${ESPGPU_CORE_DIR}/core.c"
        "${ESPGPU_CORE_DIR}/tnl.c"
        "${ESPGPU_CORE_DIR}/raster3d.c"
        "${ESPGPU_CORE_DIR}/raster2d.c"
        "${ESPGPU_CORE_DIR}/tex.c"
        "${ESPGPU_CORE_DIR}/vm.c"
    INCLUDE_DIRS "." "${ESPGPU_CORE_DIR}"
    REQUIRES espgpu_proto esp_driver_tsens esp_psram esp_timer nvs_flash esp_tinyusb
             app_update esp_pm
)

# 光栅 / T&L / VM 的内层循环对优化级别很敏感（而且现在都在 IRAM 里），单独拉到 -O3。
# 注意不能全局 -O3：IDF 自己的组件（spiffs 等）有 -Werror 会挂在 -O3 的新警告上。
set_source_files_properties(
    "${ESPGPU_CORE_DIR}/raster3d.c" "${ESPGPU_CORE_DIR}/tnl.c" "${ESPGPU_CORE_DIR}/tex.c"
    "${ESPGPU_CORE_DIR}/vm.c"
    PROPERTIES COMPILE_OPTIONS "-O3")
