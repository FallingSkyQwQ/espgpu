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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb.h"

#include "espgpu_proto.h"
#include "usb_descriptors.h"
#include "usb_device.h"

static const char *TAG = "usb";

/* TinyUSB < 0.21 的 vendor 类把 OUT 传输按 CFG_TUD_VENDOR_EPSIZE 一次性布置：
 * 主机写的长度恰好是 64 的整数倍又不带 ZLP（WinUSB 默认不带）时，最后几包会一直
 * 卡在端点缓冲里等下一次写。0.21 起 OUT 每次只收一个包，EPSIZE 只影响 IN。 */
#if defined(CFG_TUD_VENDOR_EPSIZE) && CFG_TUD_VENDOR_EPSIZE > 64 && TUSB_VERSION_NUMBER < 2100
#error "CONFIG_TINYUSB_VENDOR_EPSIZE > 64 requires TinyUSB >= 0.21 (see idf_component.yml)"
#endif

/* ── 状态 ─────────────────────────────────────────────────────────── */

static SemaphoreHandle_t s_tx_mutex;
static volatile TaskHandle_t s_rx_waiter;  /* 等待 RX 的任务 */
static volatile TaskHandle_t s_tx_waiter;  /* 等待 TX 空间的任务（持有 s_tx_mutex 的那个） */

static volatile uint32_t s_bytes_in;
static volatile uint32_t s_bytes_out;
static volatile bool s_mounted;

/* 每次 ATTACH 加一（TinyUSB 任务写）。接收 / 发送端在自己的上下文里发现它变了
 * 才清本地状态 —— 以前在 TinyUSB 任务里直接清暂存区游标，和正在解析的命令任务
 * 竞争，head > tail 时 stage_avail() 下溢，memcpy 越界。 */
static volatile uint32_t s_session;
static uint32_t s_rx_session; /* 命令任务私有 */
static uint32_t s_tx_session; /* s_tx_mutex 保护 */

/* 发送「欠账」：一个 Chunk 写到一半超时了，剩下的字节存在这里，下一次发送
 * 先把它补完再开始新 Chunk。以前是 tud_vendor_write_clear() 丢掉 FIFO，主机侧
 * 留下半截 Chunk：主机按头里的长度继续收，把后面一两个 Chunk 当成载荷吞掉
 * （D2H 不带 CRC，校验不出来），丢帧 / 丢控制响应。clear 还是在锁外调的，
 * 发送任务和命令任务并发发送时会把别人刚写进 FIFO 的半个 Chunk 一起清掉。 */
static uint8_t *s_debt_buf; /* PSRAM，HDR + MAX_PAYLOAD */
static size_t s_debt_len;
static size_t s_debt_off;

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
        /* 先登记再读：读空之后、登记之前到达的数据也能把我们叫醒 */
        s_rx_waiter = xTaskGetCurrentTaskHandle();
        uint32_t got = tud_vendor_read(s_stage + s_stage_tail, (uint32_t)room);
        if (got) {
            s_rx_waiter = NULL;
            s_stage_tail += got;
            s_bytes_in += got;
            continue;
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            s_rx_waiter = NULL;
            return ESP_ERR_TIMEOUT;
        }
        /* 短等待：优先靠 rx 回调的通知唤醒，回退 1ms 轮询 */
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
        s_rx_waiter = xTaskGetCurrentTaskHandle();
        uint32_t got = tud_vendor_read(out + done, (uint32_t)(n - done));
        if (got) {
            s_rx_waiter = NULL;
            done += got;
            s_bytes_in += got;
            continue;
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0 || !s_mounted) {
            s_rx_waiter = NULL;
            return ESP_ERR_TIMEOUT;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        s_rx_waiter = NULL;
    }
    return ESP_OK;
}

/* 头已经收到之后，载荷 / CRC 用更长的超时：中途放弃 = 丢掉一整批命令
 * （状态 / 绘制静默丢失，画面错而且没有任何报错），还要靠魔数重同步。 */
#define RX_BODY_TIMEOUT_MS 1000u

static inline uint32_t body_timeout(uint32_t timeout_ms)
{
    return timeout_ms > RX_BODY_TIMEOUT_MS ? timeout_ms : RX_BODY_TIMEOUT_MS;
}

esp_err_t espgpu_usb_read_chunk(espgpu_chunk_hdr_t *hdr, void *payload, size_t cap,
                                uint32_t timeout_ms)
{
    if (hdr == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t sess = s_session;
    if (sess != s_rx_session) {
        /* 重新枚举过：上一轮会话残留的半个 Chunk 没有意义 */
        s_rx_session = sess;
        s_stage_head = s_stage_tail = 0;
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
    esp_err_t err = stream_read(hdrbuf, sizeof(hdrbuf), body_timeout(timeout_ms));
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
        err = stream_read(payload, hdr->length, body_timeout(timeout_ms));
        if (err != ESP_OK) {
            return err;
        }
    }

    if (hdr->flags & ESPGPU_F_CRC) {
        uint16_t rx_crc = 0;
        err = stream_read(&rx_crc, sizeof(rx_crc), body_timeout(timeout_ms));
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

/** 在 deadline 之前尽量写；返回进了 FIFO 的字节数。调用者必须已持有 s_tx_mutex。 */
static size_t write_some(const uint8_t *p, size_t len, TickType_t deadline)
{
    size_t sent = 0;
    while (sent < len) {
        /* 先登记再写：FIFO 满、登记之前恰好完成的传输也能把我们叫醒。
         * 以前写满之后才登记，tx_cb 落在中间就白等 5 ms。 */
        s_tx_waiter = xTaskGetCurrentTaskHandle();
        sent += tud_vendor_write(p + sent, (uint32_t)(len - sent));
        if (sent >= len || !s_mounted || (int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            break;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    }
    s_tx_waiter = NULL;
    s_bytes_out += (uint32_t)sent;
    return sent;
}

/** 补完上一个半截 Chunk。返回 true 表示没有欠账了。持有 s_tx_mutex。 */
static bool pay_debt(TickType_t deadline)
{
    if (s_debt_len == 0) {
        return true;
    }
    s_debt_off += write_some(s_debt_buf + s_debt_off, s_debt_len - s_debt_off, deadline);
    if (s_debt_off < s_debt_len) {
        return false;
    }
    s_debt_len = s_debt_off = 0;
    return true;
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

    /* 头 / p0 / p1 依次写进 TinyUSB 的 FIFO，最后只 flush 一次：FIFO 会把它们合并成
     * 整包（满 64 字节自动起传输），效果和以前先拼进 32 KB 静态缓冲再写一样
     * （perf-notes「不是 USB 批量化」），但省掉 32 KB 内部 SRAM 和每片一次整片拷贝。 */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    const uint32_t sess = s_session;
    if (sess != s_tx_session) {
        /* 新会话：主机会按魔数重新同步，上一轮的欠账作废（总线复位时 FIFO 也清了） */
        s_tx_session = sess;
        s_debt_len = s_debt_off = 0;
    }
    if (!pay_debt(deadline)) {
        tud_vendor_write_flush();
        xSemaphoreGive(s_tx_mutex);
        return ESP_ERR_TIMEOUT; /* 上一个 Chunk 还没补完，不能插进新 Chunk */
    }

    static uint16_t s_seq;
    const espgpu_chunk_hdr_t hdr = {
        .magic = ESPGPU_MAGIC_D2H,
        .version = ESPGPU_PROTO_VERSION,
        .seq = s_seq++,
        .length = (uint16_t)len,
        .type = type,
        .flags = (uint8_t)(flags & ~ESPGPU_F_CRC),
    };

    const uint8_t *piece[3] = {(const uint8_t *)&hdr, (const uint8_t *)p0, (const uint8_t *)p1};
    const size_t plen[3] = {sizeof(hdr), n0, n1};
    size_t sent[3] = {0, 0, 0};
    bool complete = true;
    for (int k = 0; k < 3 && complete; k++) {
        if (plen[k]) {
            sent[k] = write_some(piece[k], plen[k], deadline);
            complete = sent[k] == plen[k];
        }
    }
    tud_vendor_write_flush();

    esp_err_t err = ESP_OK;
    if (!complete) {
        err = s_mounted ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
        if (sent[0] && s_debt_buf) {
            /* 头已经上路：剩下的存起来，下一次发送先补完，保证主机侧分帧不乱 */
            size_t d = 0;
            for (int k = 0; k < 3; k++) {
                if (plen[k] > sent[k]) {
                    memcpy(s_debt_buf + d, piece[k] + sent[k], plen[k] - sent[k]);
                    d += plen[k] - sent[k];
                }
            }
            s_debt_len = d;
            s_debt_off = 0;
        }
    }
    xSemaphoreGive(s_tx_mutex);
    return err;
}

esp_err_t espgpu_usb_send_ctrl_resp(uint16_t op, uint16_t seq, uint16_t status,
                                    const void *data, uint16_t len, uint32_t timeout_ms)
{
    /* 响应体 = ctrl_resp 头 + 数据，放在一次 Chunk 里，避免主机端再拼包。
     * 头在栈上、数据直接作为第二段（以前是不加锁的 static 1 KB 缓冲 + 拷贝）。 */
    if ((uint32_t)len + sizeof(espgpu_ctrl_resp_t) > ESPGPU_CHUNK_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    const espgpu_ctrl_resp_t resp = {
        .op = op,
        .seq = seq,
        .status = status,
        .len = len,
    };
    esp_err_t err = espgpu_usb_send_chunk_iov(ESPGPU_CHUNK_CTRL_RESP, 0, &resp, sizeof(resp),
                                              len ? data : NULL, len, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ctrl resp op=0x%04X seq=%u send failed: %s", op, seq,
                 esp_err_to_name(err));
    }
    return err;
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

/* FIFO 模式下 buffer/bufsize 为 NULL/0，数据在 RX FIFO 里。
 * bufsize 的类型由 CFG_TUD_API_V0_19_COMPAT 决定（默认 1 → uint16_t），
 * 签名必须和 tusb 的声明一致，否则编译报 conflicting types。 */
#if CFG_TUD_API_V0_19_COMPAT
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
#else
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint32_t bufsize)
#endif
{
    (void)idx;
    (void)buffer;
    (void)bufsize;
    TaskHandle_t t = s_rx_waiter;
    if (t) {
        xTaskNotifyGive(t);
    }
}

void tud_vendor_tx_cb(uint8_t idx, uint32_t sent_bytes)
{
    (void)idx;
    (void)sent_bytes;
    TaskHandle_t t = s_tx_waiter;
    if (t) {
        xTaskNotifyGive(t);
    }
}

/* esp_tinyusb 已经定义了 tud_mount_cb / tud_umount_cb 并转成自己的事件，
 * 所以我们通过 tinyusb_config_t.event_cb 接收挂载/卸载通知。 */
static void on_usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        /* 这里是 TinyUSB 任务：不碰命令任务的接收游标，也不拿 s_tx_mutex
         * （持锁的发送者在等本任务推进传输，拿锁会互等到超时）。
         * 只推进会话号，两边自己清；FIFO 在总线复位时已由 vendord_reset 清空。 */
        s_session = s_session + 1;
        s_mounted = true;
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

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
        ESP_LOGW(TAG, "unhandled vendor request: bRequest=0x%02X wIndex=0x%04X wLength=%u",
                 request->bRequest, request->wIndex, request->wLength);
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
    s_debt_buf = heap_caps_malloc(ESPGPU_CHUNK_HDR_SIZE + ESPGPU_CHUNK_MAX_PAYLOAD,
                                  MALLOC_CAP_SPIRAM);
    if (s_debt_buf == NULL) {
        ESP_LOGW(TAG, "no PSRAM for tx debt buffer: timed-out chunks will desync the host");
    }

    const tinyusb_config_t cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup = false,
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        /* TinyUSB 任务放 core1、优先级高于那里的所有任务（发送 7 / 光栅 worker 8）。
         * 以前在 core0、优先级 5，低于命令任务（7）：命令任务做 T&L / 光栅 / 编码的
         * 十几到二十毫秒里 tud_task 一直跑不上，USB 传输整段停摆，发送任务在 core1
         * 和下一帧重叠也就无从谈起。core1 上它只在有 USB 事件时短暂就绪。 */
        .task = {
            .size = 4096,
            .priority = 9,
            .xCoreID = 1,
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
