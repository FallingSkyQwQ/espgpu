#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "espgpu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 安装 TinyUSB 并启动设备栈。需在 espgpu_descriptors_init() 之后调用。 */
esp_err_t espgpu_usb_init(void);

/** 主机是否已枚举并打开 vendor 接口。 */
bool espgpu_usb_ready(void);

/**
 * 阻塞读取一个完整 Chunk。
 *
 * @param[out] hdr      收到的头
 * @param[out] payload  载荷缓冲区
 * @param[in]  cap      payload 容量
 * @param[in]  timeout_ms 超时
 * @return ESP_OK / ESP_ERR_TIMEOUT / ESP_ERR_INVALID_SIZE
 */
esp_err_t espgpu_usb_read_chunk(espgpu_chunk_hdr_t *hdr, void *payload, size_t cap,
                                uint32_t timeout_ms);

/** 阻塞发送一个完整 Chunk（自动加头与可选 CRC）。 */
esp_err_t espgpu_usb_send_chunk(uint8_t type, uint8_t flags, const void *payload,
                                uint32_t len, uint32_t timeout_ms);

/**
 * 分段发送一个 Chunk：载荷由 [p0,n0] 与 [p1,n1] 两段拼成。
 * 用于把帧头和像素数据直接拼进同一个 Chunk，省掉一次大块内存拷贝。
 */
esp_err_t espgpu_usb_send_chunk_iov(uint8_t type, uint8_t flags, const void *p0, uint32_t n0,
                                    const void *p1, uint32_t n1, uint32_t timeout_ms);

/** 直接写入原始字节（发送大块载荷时避免再拷一层）。 */
size_t espgpu_usb_write(const void *data, size_t len, uint32_t timeout_ms);

/** 发送一个控制响应。 */
esp_err_t espgpu_usb_send_ctrl_resp(uint16_t op, uint16_t seq, uint16_t status,
                                    const void *data, uint16_t len, uint32_t timeout_ms);

/** 发送一个事件。 */
esp_err_t espgpu_usb_send_event(uint8_t kind, uint8_t code, uint16_t value, uint32_t arg,
                                uint32_t timeout_ms);

/** 已发送/接收字节计数（供统计使用）。 */
uint32_t espgpu_usb_bytes_in(void);
uint32_t espgpu_usb_bytes_out(void);

#ifdef __cplusplus
}
#endif
