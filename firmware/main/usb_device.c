/*
 * usb_device.c — vendor 类传输层
 *
 * 两条 bulk 端点承载全部流量：
 *   EP1 OUT  host -> device   命令 / 控制请求 / 数据上传
 *   EP2 IN   device -> host   帧 / 事件 / 控制响应 / 日志
 *
 * Chunk 分帧在 espgpu_proto.h 里定义。这里只负责：
 *   - 从字节流里切出完整 Chunk（含坏帧重同步）
 *   - 把 Chunk 打包发出去（带背压等待）
 *   - 响应 MS OS 2.0 的 vendor 控制请求，让 Windows 免驱绑定 WinUSB
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb.h"

#include "espgpu_proto.h"
#include "usb_descriptors.h"
#include "usb_device.h"

static const char *TAG = "usb";

/* ── 状态 ─────────────────────────────────────────────────────────── */

static SemaphoreHandle_t s_tx_mutex;
static TaskHandle_t s_rx_waiter;  /* 等待 RX 的任务 */
static TaskHandle_t s_tx_waiter;  /* 等待 TX 空间的任务 */

static volatile uint32_t s_bytes_in;
static volatile uint32_t s_bytes_out;
static volatile bool s_mounted;

/* 接收暂存区。只需容纳头部的滑动窗口，大载荷直接读进调用者的缓冲区。 */
#define RX_STAGE 256
static uint8_t s_stage[RX_STAGE];
static size_t s_stage_head;
static size_t s_stage_tail;

/* ── 接收 ─────────────────────────────────────────────────────────── */

static inline size_t stage_avail(void)
{
    return s_stage_tail - s_stage_head;
}

static void stage_compact(void)
{
    if (s_stage_head == 0) {
        return;
    }
    size_t n = stage_avail();
    if (n) {
        memmove(s_stage, s_stage + s_stage_head, n);
    }
    s_stage_head = 0;
    s_stage_tail = n;
}

/**
 * 保证暂存区里至少有 n 字节。超时返回 ESP_ERR_TIMEOUT。
 * n 必须 <= RX_STAGE。
 */
static esp_err_t stage_need(size_t n, uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (stage_avail() < n) {
        stage_compact();
        size_t room = RX_STAGE - s_stage_tail;
        if (room == 0) {
            return ESP_ERR_INVALID_SIZE; /* 不可能发生：n <= RX_STAGE */
        }
        uint32_t got = tud_vendor_read(s_stage + s_stage_tail, (uint32_t)room);
        if (got) {
            s_stage_tail += got;
            s_bytes_in += got;
            continue;
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        /* 短等待：优先靠 rx 回调的通知唤醒，回退 1ms 轮询 */
        s_rx_waiter = xTaskGetCurrentTaskHandle();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        s_rx_waiter = NULL;
    }
    return ESP_OK;
}

/** 从字节流里精确取 n 字节到 dst（n 可以大于 RX_STAGE）。 */
static esp_err_t stream_read(void *dst, size_t n, uint32_t timeout_ms)
{
    uint8_t *out = (uint8_t *)dst;
    size_t done = 0;

    /* 先排空暂存区 */
    size_t take = stage_avail();
    if (take > n) {
        take = n;
    }
    if (take) {
        memcpy(out, s_stage + s_stage_head, take);
        s_stage_head += take;
        done += take;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (done < n) {
        uint32_t got = tud_vendor_read(out + done, (uint32_t)(n - done));
        if (got) {
            done += got;
            s_bytes_in += got;
            continue;
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        s_rx_waiter = xTaskGetCurrentTaskHandle();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        s_rx_waiter = NULL;
    }
    return ESP_OK;
}

esp_err_t espgpu_usb_read_chunk(espgpu_chunk_hdr_t *hdr, void *payload, size_t cap,
                                uint32_t timeout_ms)
{
    if (hdr == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 用 4 字节滑动窗口找魔数，避免上电/异常后一直错位 */
    uint32_t magic = 0;
    for (;;) {
        esp_err_t err = stage_need(4, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        memcpy(&magic, s_stage + s_stage_head, 4);
        if (magic == ESPGPU_MAGIC_H2D) {
            break;
        }
        s_stage_head++; /* 错位一字节继续找 */
    }

    uint8_t hdrbuf[ESPGPU_CHUNK_HDR_SIZE];
    esp_err_t err = stream_read(hdrbuf, sizeof(hdrbuf), timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(hdr, hdrbuf, sizeof(hdrbuf));

    if (hdr->version != ESPGPU_PROTO_VERSION) {
        ESP_LOGW(TAG, "proto version mismatch: dev=%u host=%u", ESPGPU_PROTO_VERSION,
                 hdr->version);
        return ESP_ERR_INVALID_VERSION;
    }
    if (hdr->length > cap) {
        ESP_LOGW(TAG, "chunk too large: %u > %u", hdr->length, (unsigned)cap);
        return ESP_ERR_INVALID_SIZE;
    }
    if (hdr->length) {
        err = stream_read(payload, hdr->length, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (hdr->flags & ESPGPU_F_CRC) {
        uint16_t rx_crc = 0;
        err = stream_read(&rx_crc, sizeof(rx_crc), timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        uint16_t calc = espgpu_chunk_crc(hdr, payload);
        if (rx_crc != calc) {
            ESP_LOGW(TAG, "crc mismatch: rx=%04x calc=%04x", rx_crc, calc);
            return ESP_ERR_INVALID_CRC;
        }
    }

    return ESP_OK;
}

/* ── 发送 ─────────────────────────────────────────────────────────── */

/** 调用者必须已持有 s_tx_mutex。 */
static size_t write_locked(const void *data, size_t len, uint32_t timeout_ms)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (sent < len) {
        uint32_t n = tud_vendor_write(p + sent, (uint32_t)(len - sent));
        sent += n;
        if (sent >= len) {
            break;
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            break;
        }
        s_tx_waiter = xTaskGetCurrentTaskHandle();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
        s_tx_waiter = NULL;
    }
    tud_vendor_write_flush();
    s_bytes_out += (uint32_t)sent;
    return sent;
}

size_t espgpu_usb_write(const void *data, size_t len, uint32_t timeout_ms)
{
    if (!s_mounted) {
        return 0;
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    size_t n = write_locked(data, len, timeout_ms);
    xSemaphoreGive(s_tx_mutex);
    return n;
}

esp_err_t espgpu_usb_send_chunk(uint8_t type, uint8_t flags, const void *payload,
                                uint32_t len, uint32_t timeout_ms)
{
    return espgpu_usb_send_chunk_iov(type, flags, payload, len, NULL, 0, timeout_ms);
}

esp_err_t espgpu_usb_send_chunk_iov(uint8_t type, uint8_t flags, const void *p0, uint32_t n0,
                                    const void *p1, uint32_t n1, uint32_t timeout_ms)
{
    const uint32_t len = n0 + n1;
    if (len > ESPGPU_CHUNK_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

    /* seq 必须在锁内自增：命令任务和呈现任务都会发 Chunk */
    static uint16_t s_seq;

    espgpu_chunk_hdr_t hdr = {
        .magic = ESPGPU_MAGIC_D2H,
        .version = ESPGPU_PROTO_VERSION,
        .seq = s_seq++,
        .length = (uint16_t)len,
        .type = type,
        .flags = (uint8_t)(flags & ~ESPGPU_F_CRC),
    };

    size_t got = write_locked(&hdr, sizeof(hdr), timeout_ms);
    if (got != sizeof(hdr)) {
        /* 头都没写完：丢掉 FIFO 里残留，让主机靠魔数重同步 */
        tud_vendor_write_clear();
        xSemaphoreGive(s_tx_mutex);
        return ESP_ERR_TIMEOUT;
    }

    if (n0 && write_locked(p0, n0, timeout_ms) != n0) {
        tud_vendor_write_clear();
        xSemaphoreGive(s_tx_mutex);
        return ESP_ERR_TIMEOUT;
    }
    if (n1 && write_locked(p1, n1, timeout_ms) != n1) {
        tud_vendor_write_clear();
        xSemaphoreGive(s_tx_mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(s_tx_mutex);
    return ESP_OK;
}

esp_err_t espgpu_usb_send_ctrl_resp(uint16_t op, uint16_t seq, uint16_t status,
                                   const void *data, uint16_t len, uint32_t timeout_ms)
{
    /* 响应体 = ctrl_resp 头 + 数据，放在一次 Chunk 里，避免主机端再拼包 */
    static uint8_t buf[1024];
    if (len > sizeof(buf) - sizeof(espgpu_ctrl_resp_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    espgpu_ctrl_resp_t *resp = (espgpu_ctrl_resp_t *)buf;
    resp->op = op;
    resp->seq = seq;
    resp->status = status;
    resp->len = len;
    if (len) {
        memcpy(buf + sizeof(*resp), data, len);
    }
    return espgpu_usb_send_chunk(ESPGPU_CHUNK_CTRL_RESP, 0, buf,
                                 (uint32_t)(sizeof(*resp) + len), timeout_ms);
}

esp_err_t espgpu_usb_send_event(uint8_t kind, uint8_t code, uint16_t value, uint32_t arg,
                                uint32_t timeout_ms)
{
    espgpu_event_t ev = {.kind = kind, .code = code, .value = value, .arg = arg};
    return espgpu_usb_send_chunk(ESPGPU_CHUNK_EVENT, 0, &ev, sizeof(ev), timeout_ms);
}

uint32_t espgpu_usb_bytes_in(void)
{
    return s_bytes_in;
}

uint32_t espgpu_usb_bytes_out(void)
{
    return s_bytes_out;
}

bool espgpu_usb_ready(void)
{
    return s_mounted && tud_vendor_mounted();
}

/* ── TinyUSB 回调 ────────────────────────────────────────────────── */

void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
{
    (void)idx;
    (void)buffer;
    (void)bufsize;
    if (s_rx_waiter) {
        xTaskNotifyGive(s_rx_waiter);
    }
}

void tud_vendor_tx_cb(uint8_t idx, uint32_t sent_bytes)
{
    (void)idx;
    (void)sent_bytes;
    if (s_tx_waiter) {
        xTaskNotifyGive(s_tx_waiter);
    }
}

/* esp_tinyusb 已经定义了 tud_mount_cb / tud_umount_cb 并转成自己的事件，
 * 所以我们通过 tinyusb_config_t.event_cb 接收挂载/卸载通知。 */
static void on_usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_mounted = true;
        s_stage_head = s_stage_tail = 0;
        ESP_LOGI(TAG, "attached (enumerated)");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        s_mounted = false;
        ESP_LOGW(TAG, "detached");
    }
}

/**
 * vendor 控制请求。
 *
 * Windows 在枚举后（以及每次设备重启后）会发：
 *   bmRequestType = 0xC0 (IN | VENDOR | DEVICE)
 *   bRequest      = bMS_VendorCode = 0x01
 *   wIndex        = 0x0007 (MS_OS_20_DESCRIPTOR_INDEX)
 *   wLength       = 38
 * 我们把 MS OS 2.0 descriptor set 回给它，Windows 据此自动加载 WinUSB。
 *
 * 其它 vendor 请求一律 STALL —— 正常的数据通路走 bulk 端点，不占用控制端点。
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true; /* 已由 tud_control_xfer 接管后续阶段 */
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == ESPGPU_MSOS_VENDOR_CODE &&
        tu_u16_low(request->wIndex) == ESPGPU_MSOS_DESC_INDEX) {
        uint16_t len = tu_min16(request->wLength, espgpu_msos_desc_set_len);
        ESP_LOGI(TAG, "MS OS 2.0 descriptor requested (%u bytes)", len);
        return tud_control_xfer(rhport, request, (void *)espgpu_msos_desc_set, len);
    }

    return false;
}

/* ── 初始化 ──────────────────────────────────────────────────────── */

esp_err_t espgpu_usb_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    if (s_tx_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const tinyusb_config_t cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup = false,
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = 4096,
            .priority = 5,
            .xCoreID = 0,
        },
        .descriptor = {
            .device = &espgpu_device_desc,
            .qualifier = NULL,
            .string = espgpu_string_desc,
            .string_count = ESPGPU_STRING_DESC_COUNT,
            .full_speed_config = espgpu_config_desc,
            .high_speed_config = NULL,
        },
        .pm_lock_enable = false,
        .event_cb = on_usb_event,
        .event_arg = NULL,
    };

    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "TinyUSB installed, VID=%04X PID=%04X", ESPGPU_USB_VID, ESPGPU_USB_PID);
    return ESP_OK;
}
