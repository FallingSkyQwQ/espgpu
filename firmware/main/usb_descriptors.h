#pragma once

#include <stdint.h>
#include "tusb.h"
#include "espgpu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 字符串索引 */
enum {
    ESPGPU_STRID_LANGID = 0,
    ESPGPU_STRID_MANUFACTURER = 1,
    ESPGPU_STRID_PRODUCT = 2,
    ESPGPU_STRID_SERIAL = 3,
    ESPGPU_STRING_DESC_COUNT = 4,
};

/* MS OS 2.0 的 vendor code 与 descriptor index */
#define ESPGPU_MSOS_VENDOR_CODE 0x01u
#define ESPGPU_MSOS_DESC_INDEX 0x0007u

extern tusb_desc_device_t espgpu_device_desc;
extern const uint8_t espgpu_config_desc[];
extern const uint8_t espgpu_bos_desc[];
extern const uint8_t espgpu_msos_desc_set[];
extern const uint16_t espgpu_msos_desc_set_len;
extern const char *espgpu_string_desc[ESPGPU_STRING_DESC_COUNT];

/** 置位设备描述符里的诊断位（见 usb_descriptors.c 的说明）。 */
void espgpu_desc_diag(uint16_t bits);

/** 用芯片 MAC 生成序列号，必须在 tinyusb_driver_install() 之前调用。 */
void espgpu_descriptors_init(void);

#ifdef __cplusplus
}
#endif
