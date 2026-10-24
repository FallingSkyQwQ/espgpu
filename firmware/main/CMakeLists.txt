idf_component_register(
    SRCS
        "app_main.c"
        "usb_descriptors.c"
        "usb_device.c"
        "framebuffer.c"
        "gpu.c"
    INCLUDE_DIRS "."
    REQUIRES espgpu_proto esp_driver_tsens esp_psram esp_timer nvs_flash esp_tinyusb
)
