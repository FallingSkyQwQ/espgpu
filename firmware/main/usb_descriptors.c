/*
 * usb_descriptors.c — USB 描述符与 WinUSB 免驱（WCID / MS OS 2.0）
 *
 * 关键点：
 *  - BOS 里带 MS OS 2.0 platform capability，bMS_VendorCode = 0x01
 *  - 设备响应 vendor request 0x01 / wIndex=0x07，返回 MS OS 2.0 descriptor set
 *  - descriptor set 里声明 Compatible ID = "WINUSB"
 *  - Windows 10/11 据此自动绑定 winusb.sys，无需 .inf、无需签名
 *
 * 描述符一律手写字节数组：字段顺序与线上格式一一对应，便于拿 USB 抓包工具比对，
 * 也避免依赖具体 TinyUSB 版本的宏命名。
 */

#include <string.h>
#include <stdio.h>

#include "tusb.h"
#include "esp_mac.h"
#include "usb_descriptors.h"

/* ── 标识 ─────────────────────────────────────────────────────────── */

/* 0x303A 是 USB-IF 分配给 Espressif 的 VID，PID 在其范围内自选，见 usb_descriptors.h */

/* ── 端点 ─────────────────────────────────────────────────────────── */

#define ESPGPU_EP_CMD_OUT 0x01 /* EP1 OUT — host -> device，命令流 */
#define ESPGPU_EP_FRM_IN 0x82  /* EP2 IN  — device -> host，帧流 */
#define ESPGPU_EP_MPS 64u      /* Full-Speed bulk 最大包长 */

/* ── 设备描述符 ───────────────────────────────────────────────────── */

/* 非 const：bcdDevice 的低字节被用作启动诊断位，见 espgpu_desc_diag()。
 * 枚举期间 Windows 会重新读取设备描述符，所以把这些位改掉之后
 * 只要重新扫描一次设备就能从 REV_xxxx 里读出来。
 *
 *   bit4 (0x10)  Windows 请求了 BOS 描述符
 *   bit0 (0x01)  收到了匹配的 MS OS 2.0 vendor 请求并已应答
 *   bit1 (0x02)  收到了 vendor 请求但参数不匹配（被 STALL）
 */
tusb_desc_device_t espgpu_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210, /* USB 2.1：BOS 描述符要求 bcdUSB >= 2.01 */
    /* 类信息定义在接口层，设备级留 0，Windows 走 per-interface 匹配 */
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = ESPGPU_USB_VID,
    .idProduct = ESPGPU_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = ESPGPU_STRID_MANUFACTURER,
    .iProduct = ESPGPU_STRID_PRODUCT,
    .iSerialNumber = ESPGPU_STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

void espgpu_desc_diag(uint16_t bits)
{
    espgpu_device_desc.bcdDevice |= bits;
}

/* ── 配置描述符 ───────────────────────────────────────────────────── */

enum { ESPGPU_CONFIG_TOTAL_LEN = 9 + 9 + 7 + 7 }; /* 32 */

const uint8_t espgpu_config_desc[] = {
    /* ── 配置描述符 ── */
    0x09, 0x02,                                     /* bLength, CONFIGURATION */
    ESPGPU_CONFIG_TOTAL_LEN & 0xFF,
    ESPGPU_CONFIG_TOTAL_LEN >> 8,                   /* wTotalLength = 32 */
    0x01,                                           /* bNumInterfaces */
    0x01,                                           /* bConfigurationValue */
    0x00,                                           /* iConfiguration */
    0x80,                                           /* bmAttributes: bus powered */
    250,                                            /* bMaxPower: 500 mA */

    /* ── 接口 0：vendor specific ── */
    0x09, 0x04,                                     /* bLength, INTERFACE */
    0x00,                                           /* bInterfaceNumber */
    0x00,                                           /* bAlternateSetting */
    0x02,                                           /* bNumEndpoints */
    0xFF,                                           /* bInterfaceClass = VENDOR_SPECIFIC */
    0x00,                                           /* bInterfaceSubClass */
    0x00,                                           /* bInterfaceProtocol */
    0x00,                                           /* iInterface */

    /* ── EP1 OUT：命令流 ── */
    0x07, 0x05,                                     /* bLength, ENDPOINT */
    ESPGPU_EP_CMD_OUT,                              /* bEndpointAddress */
    0x02,                                           /* bmAttributes = BULK */
    ESPGPU_EP_MPS & 0xFF, ESPGPU_EP_MPS >> 8,       /* wMaxPacketSize */
    0x00,                                           /* bInterval */

    /* ── EP2 IN：帧流 ── */
    0x07, 0x05,                                     /* bLength, ENDPOINT */
    ESPGPU_EP_FRM_IN,                               /* bEndpointAddress */
    0x02,                                           /* bmAttributes = BULK */
    ESPGPU_EP_MPS & 0xFF, ESPGPU_EP_MPS >> 8,       /* wMaxPacketSize */
    0x00,                                           /* bInterval */
};

/* ── BOS + MS OS 2.0 platform capability ──────────────────────────── */

enum { ESPGPU_BOS_TOTAL_LEN = 5 + 28 }; /* 33 */

const uint8_t espgpu_bos_desc[] = {
    /* ── BOS 头 ── */
    0x05, 0x0F,                                     /* bLength, BOS */
    ESPGPU_BOS_TOTAL_LEN & 0xFF,
    ESPGPU_BOS_TOTAL_LEN >> 8,                      /* wTotalLength = 33 */
    0x01,                                           /* bNumDeviceCaps */

    /* ── Platform capability ── */
    0x1C, 0x10,                                     /* bLength = 28, DEVICE_CAPABILITY */
    0x05,                                           /* bDevCapabilityType = PLATFORM */
    0x00,                                           /* bReserved */

    /* PlatformCapabilityUUID = {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F} */
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,

    0x00, 0x00, 0x03, 0x06,                         /* dwWindowsVersion = 0x06030000 */
    0x1E, 0x00,                                     /* wMSOSDescriptorSetTotalLength = 30 */
    ESPGPU_MSOS_VENDOR_CODE,                        /* bMS_VendorCode = 0x01 */
    0x00,                                           /* bAltEnumCode */
};

/* ── MS OS 2.0 descriptor set ─────────────────────────────────────── */
/*
 * 本设备是「非复合设备」：bDeviceClass = 0x00、单接口、不使用 Usbccgp.sys。
 *
 * 微软 MS OS 2.0 规范明确规定：
 *   - configuration subset header 只用于加载 Usbccgp.sys 的复合设备
 *   - function subset header 只用于复合设备，或使用 Usbccgp.sys 的单功能设备
 *
 * "非复合设备 + 单个 feature descriptor" 的标准形态就是
 * 「set header + compatible ID」的扁平结构，不能再套 subset header。
 *
 * 踩过的坑：套了 configuration subset 之后，Windows 会**静默忽略**其中的
 * compatible ID —— MS OS 2.0 请求照常应答（日志正常），但设备永远拿不到
 * USB\MS_COMP_WINUSB，最终表现为设备管理器 Code 28。
 */
enum {
    ESPGPU_MSOS_HEADER_LEN = 10,
    ESPGPU_MSOS_COMPAT_ID_LEN = 20,
    ESPGPU_MSOS_TOTAL_LEN = ESPGPU_MSOS_HEADER_LEN + ESPGPU_MSOS_COMPAT_ID_LEN, /* 30 */
};

const uint8_t espgpu_msos_desc_set[] = {
    /* Set header */
    0x0A, 0x00,                                     /* wLength = 10 */
    0x00, 0x00,                                     /* MS_OS_20_SET_HEADER_DESCRIPTOR */
    0x00, 0x00, 0x03, 0x06,                         /* dwWindowsVersion = 0x06030000 */
    ESPGPU_MSOS_TOTAL_LEN & 0xFF,
    ESPGPU_MSOS_TOTAL_LEN >> 8,                     /* wTotalLength = 30 */

    /* Compatible ID（20 字节，规范规定无 bFirstInterfaceNumber 字段） */
    0x14, 0x00,                                     /* wLength = 20 */
    0x03, 0x00,                                     /* MS_OS_FEATURE_COMPATIBLE_ID */
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,       /* CompatibleID[8] = "WINUSB" */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* SubCompatibleID[8] */
};

const uint16_t espgpu_msos_desc_set_len = ESPGPU_MSOS_TOTAL_LEN;

_Static_assert(sizeof(espgpu_config_desc) == ESPGPU_CONFIG_TOTAL_LEN,
               "config descriptor length mismatch");
_Static_assert(sizeof(espgpu_bos_desc) == ESPGPU_BOS_TOTAL_LEN,
               "BOS descriptor length mismatch");
_Static_assert(sizeof(espgpu_msos_desc_set) == ESPGPU_MSOS_TOTAL_LEN,
               "MS OS 2.0 descriptor set length mismatch");
_Static_assert(ESPGPU_MSOS_TOTAL_LEN < 256, "MS OS total length must fit in one packet");

/* ── 字符串描述符 ─────────────────────────────────────────────────── */

static char s_serial[24] = "ESPGPU-000000";

const char *espgpu_string_desc[ESPGPU_STRING_DESC_COUNT] = {
    "\x09\x04",    /* index 0：LANGID en-US，esp_tinyusb 约定前 2 字节 */
    "Falling Sky", /* 1 - manufacturer */
    "ESP32-S3 GPU",/* 2 - product */
    s_serial,      /* 3 - serial */
};

void espgpu_descriptors_init(void)
{
    /* 直接读 eFuse，不需要初始化 WiFi/BT */
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_serial, sizeof(s_serial), "ESPGPU-%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);
}

/* ── TinyUSB 描述符回调 ──────────────────────────────────────────── */

/* device / configuration / string 三个回调由 esp_tinyusb 提供，这里只补 BOS。 */
const uint8_t *tud_descriptor_bos_cb(void)
{
    return espgpu_bos_desc;
}
