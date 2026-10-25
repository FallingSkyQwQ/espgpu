/*
 * gpu.c — 设备状态、命令分发、帧发送、管理通道
 *
 * 两条出帧路径：
 *   M1 测试图案：呈现任务（core1）生成图案 → 编码 → 分片发送
 *   命令模式   ：命令任务（core0）执行命令流，交给 firmware/core 做 T&L + tile 光栅，
 *                PRESENT 时编码，再把编码结果交给发送任务。发送（~9 ms）与下一帧的
 *                命令执行重叠，控制请求也不用再排在整帧发送之后（perf-notes 第 3 条）。
 *
 * 管理通道（M5）：设备信息 / 统计 / 模式 / 时钟 / 调试视图 / OTA / 日志订阅 / 软复位。
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"

#include "espgpu_block.h"
#include "espgpu_lz.h"
#include "espgpu_proto.h"
#include "frame_chain.h"
#include "gpu.h"
#include "log_fwd.h"
#include "usb_device.h"
#include "version.h"

static const char *TAG = "gpu";

espgpu_gpu_t g_gpu;

/* ── 接收任务 → 命令任务（issue #3） ──
 * 以前命令任务自己读 USB：读到一个 CMD chunk 就把整帧执行完（T&L + 光栅 + 编码，
 * 3D 一帧 100+ ms）才回去读下一个 chunk，排在后面的 GET_STATS 等控制请求必然超时。
 * 现在接收任务（core0，优先级高于命令任务）只管读 chunk：控制请求当场处理并回响应，
 * CMD / DATA（以及要动渲染状态的 SET_MODE / SET_DEBUG_VIEW）按到达顺序进队列给命令任务。 */
#define RX_POOL 4
enum { RX_CMD = 0, RX_DATA = 1, RX_CTRL = 2 };
typedef struct {
    espgpu_chunk_hdr_t hdr;
    uint8_t kind;
    uint8_t buf; /* s_rx_buf 下标 */
} rx_item_t;
static uint8_t *s_rx_buf[RX_POOL];     /* 每个一整个 chunk 载荷（PSRAM） */
static QueueHandle_t s_rx_free_q;      /* 空闲缓冲下标 */
static QueueHandle_t s_rx_work_q;      /* 待命令任务处理的 rx_item_t */
static uint8_t *s_lz_buf;               /* LZ 解压缓冲 */
#define LZ_BUF_SIZE (64u * 1024u)
static temperature_sensor_handle_t s_tsens;
static int64_t s_boot_us;

#define TEXTURE_BUDGET (4u * 1024u * 1024u)
#define ARENA_BYTES (1024u * 1024u)
#define BUFFER_BUDGET (512u * 1024u)

/* ── 发送队列（命令模式） ─────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;
    size_t cap;
    uint32_t enc_len;
    uint32_t raw_len;
    uint32_t frame_id;
    uint16_t w, h;
    uint8_t codec;
    bool key;      /**< 关键帧（主机不需要参考帧就能解） */
} send_slot_t;

#define SEND_SLOTS 2
static send_slot_t s_slots[SEND_SLOTS];
static QueueHandle_t s_free_q; /* 可写槽位下标 */
static QueueHandle_t s_send_q; /* 待发槽位下标 */

static chain_tx_t s_chain_tx;     /* 发送任务写 */
static chain_prod_t s_chain_prod; /* 命令任务写 */

/* 一帧最多几个分片：frag_index / frag_count 是 u8，主机按 255 片重组。
 * 不用 ESPGPU_MAX_FRAGMENTS（16）：800x600 RAW 要 30 片，大分辨率下不可压缩的
 * RLE 帧也会超过 16 片，以前这些帧永远发不出去。 */
#define FRAME_MAX_FRAGMENTS 255u

/* 读-清 一次完成：避免「读到 false → 别的任务置 true → 这里写 false」丢掉请求 */
static inline bool take_keyframe_request(void)
{
    return __atomic_exchange_n(&g_gpu.force_keyframe, 0u, __ATOMIC_ACQ_REL) != 0;
}

/* ── OTA ──────────────────────────────────────────────────────────── */

static struct {
    esp_ota_handle_t handle;
    const esp_partition_t *part;
    uint32_t total;
    uint32_t written;
    uint32_t crc;
    uint8_t state; /* 0 空闲 1 写入中 2 待重启 3 失败 */
} s_ota;

/* ── 统计 ─────────────────────────────────────────────────────────── */

void espgpu_gpu_sample_stats(void)
{
    espgpu_stats_t *s = &g_gpu.stats;

    s->uptime_ms = (uint32_t)((esp_timer_get_time() - s_boot_us) / 1000);
    s->bytes_in = espgpu_usb_bytes_in();
    s->bytes_out = espgpu_usb_bytes_out();
    s->free_heap = esp_get_free_heap_size();
    s->min_free_heap = esp_get_minimum_free_heap_size();
    s->free_psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s->cpu_freq_mhz = g_gpu.cpu_mhz;
    s->ota_state = s_ota.state;
    s->ota_written = s_ota.written;

    if (s_tsens) {
        float c = 0.0f;
        if (temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) {
            s->internal_temp_c10 = (int16_t)(c * 10.0f);
        }
    }
}

/* ── 设备信息 ─────────────────────────────────────────────────────── */

static void fill_device_info(espgpu_device_info_t *di)
{
    memset(di, 0, sizeof(*di));

    di->proto_version = ESPGPU_PROTO_VERSION;
    di->fw_major = ESPGPU_FW_VERSION_MAJOR;
    di->fw_minor = ESPGPU_FW_VERSION_MINOR;
    di->fw_patch = ESPGPU_FW_VERSION_PATCH;
    di->fw_git = ESPGPU_FW_GIT;

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    snprintf(di->chip_model, sizeof(di->chip_model), "ESP32-S3 r%u.%u", ci.revision / 100,
             ci.revision % 100);

    di->max_width = 800;
    di->max_height = 600;
    di->min_width = 32;
    di->min_height = 32;

    size_t psram = esp_psram_get_size();
    di->psram_total = (uint32_t)psram;
    di->psram_mb = (uint8_t)(psram / (1024 * 1024));
    di->flash_mb = 16;
    di->psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    di->sram_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    di->sram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    const esp_partition_t *run = esp_ota_get_running_partition();
    di->flash_total = 16u * 1024u * 1024u;
    di->flash_used = run ? run->size : 0;

    di->capabilities = ESPGPU_CAP_INDEX8 | ESPGPU_CAP_RGB565 | ESPGPU_CAP_DEPTH |
                       ESPGPU_CAP_FBO | ESPGPU_CAP_FRAGMENT_VM | ESPGPU_CAP_QUADS |
                       ESPGPU_CAP_LIGHTING | ESPGPU_CAP_OTA | ESPGPU_CAP_TNL |
                       ESPGPU_CAP_TEX_LRU | ESPGPU_CAP_PALETTED_TEX | ESPGPU_CAP_BUFFERS |
                       ESPGPU_CAP_CLOCK | ESPGPU_CAP_DEBUG_VIEW | ESPGPU_CAP_LZ |
                       ESPGPU_CAP_LOG;
    di->codecs = ESPGPU_CAP_CODEC_RAW | ESPGPU_CAP_CODEC_RLE | ESPGPU_CAP_CODEC_DELTA_RLE |
                 ESPGPU_CAP_CODEC_BLOCK | ESPGPU_CAP_CODEC_AUTO;

    di->texture_cache_size = TEXTURE_BUDGET;
    di->max_texture_dim = 512;
    di->max_texture_units = ESPGPU_MAX_TEXTURE_UNITS;
    di->max_vertex_attribs = ESPGPU_MAX_VERTEX_ATTRIBS;
    di->cpu_freq_mhz = (uint8_t)(g_gpu.cpu_mhz > 255 ? 240 : g_gpu.cpu_mhz);
    di->psram_freq_mhz = 80;
}

/* ── 模式 ─────────────────────────────────────────────────────────── */

static esp_err_t realloc_encode_buffer(void)
{
    size_t cap = espgpu_fb_max_encoded(&g_gpu.fb);
    if (g_gpu.enc_buf && g_gpu.enc_cap >= cap) {
        return ESP_OK;
    }
    if (g_gpu.enc_buf) {
        heap_caps_free(g_gpu.enc_buf);
        g_gpu.enc_buf = NULL;
    }
    g_gpu.enc_buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (g_gpu.enc_buf == NULL) {
        ESP_LOGE(TAG, "encode buffer alloc failed (%u bytes)", (unsigned)cap);
        g_gpu.enc_cap = 0;
        return ESP_ERR_NO_MEM;
    }
    g_gpu.enc_cap = cap;
    return ESP_OK;
}

static espgpu_mode_t sanitize_mode(const espgpu_mode_t *mode)
{
    espgpu_mode_t m = *mode;
    if (m.width < 32) m.width = 32;
    if (m.height < 32) m.height = 32;
    if (m.width > 800) m.width = 800;
    if (m.height > 600) m.height = 600;
    if (m.max_fps == 0) m.max_fps = 24;
    if (m.max_fps > 60) m.max_fps = 60;
    if (m.format != ESPGPU_FMT_INDEX8 && m.format != ESPGPU_FMT_RGB565) {
        m.format = ESPGPU_FMT_RGB565;
    }
    if (m.codec > ESPGPU_CODEC_DELTA_RLE) {
        m.codec = ESPGPU_CODEC_DELTA_RLE;
    }
    return m;
}

/* 调用者必须持有 g_gpu.lock。只管测试图案路径的帧缓冲。 */
static esp_err_t apply_mode_locked(const espgpu_mode_t *mode, uint8_t pattern)
{
    espgpu_mode_t m = sanitize_mode(mode);

    esp_err_t err = ESP_OK;
    if (m.width != g_gpu.fb.width || m.height != g_gpu.fb.height ||
        m.format != g_gpu.fb.format) {
        espgpu_fb_deinit(&g_gpu.fb);
        err = espgpu_fb_init(&g_gpu.fb, m.width, m.height, m.format);
        if (err == ESP_OK) {
            err = realloc_encode_buffer();
        }
        if (err != ESP_OK) {
            /* 回退到安全模式，避免设备变砖 */
            espgpu_fb_init(&g_gpu.fb, 320, 200, ESPGPU_FMT_RGB565);
            realloc_encode_buffer();
            m.width = 320;
            m.height = 200;
            m.format = ESPGPU_FMT_RGB565;
            err = ESP_ERR_NO_MEM;
        }
    }

    g_gpu.mode = m;
    g_gpu.pattern = pattern < ESPGPU_PATTERN_COUNT ? pattern : ESPGPU_PATTERN_DOOMISH;
    g_gpu.present_period_us = 1000000u / m.max_fps;

    /* 分辨率/格式/图案一变，主机端的差分参考帧就失效了。
     * 强制下一帧走非差分编码，让主机重建参考帧，否则会解出花屏。 */
    g_gpu.force_keyframe = true;

    ESP_LOGI(TAG, "mode %ux%u fmt=%u codec=%u fps=%u pattern=%u", m.width, m.height,
             m.format, m.codec, m.max_fps, g_gpu.pattern);
    return err;
}

esp_err_t espgpu_gpu_set_mode(const espgpu_mode_t *mode, uint8_t pattern)
{
    if (mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 呈现任务整帧持有 g_gpu.lock，控制任务直接去 take 会被饿死
     * （实测 SET_MODE 阻塞 15 秒，主机侧 800ms 就超时了）。
     * 所以这里只挂一个待办，由呈现任务在下一个帧边界自己应用。 */
    g_gpu.pending_mode = *mode;
    g_gpu.pending_pattern = pattern;
    g_gpu.mode_pending = true;

    for (int i = 0; i < 500; i++) {
        if (!g_gpu.mode_pending) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_ERR_TIMEOUT;
}

void espgpu_gpu_set_clut(const uint16_t *clut)
{
    if (clut == NULL) {
        return;
    }

    uint16_t copy[ESPGPU_CLUT_ENTRIES];
    memcpy(copy, clut, sizeof(copy));

    xSemaphoreTake(g_gpu.lock, portMAX_DELAY);
    memcpy(g_gpu.clut, copy, sizeof(copy));
    xSemaphoreGive(g_gpu.lock);

    /* 把权威调色板推回主机，保证双方对 INDEX8 的解释一致 */
    espgpu_usb_send_chunk(ESPGPU_CHUNK_CLUT, 0, copy, sizeof(copy), 200);
}

/* ── 帧发送 ───────────────────────────────────────────────────────── */

static esp_err_t send_frame(const uint8_t *enc, uint32_t enc_len, uint32_t raw_len,
                            uint8_t codec, uint16_t w, uint16_t h, uint8_t format,
                            uint32_t frame_id)
{
    const uint32_t hdr_sz = (uint32_t)sizeof(espgpu_frame_hdr_t);
    const uint32_t frag_cap = ESPGPU_CHUNK_MAX_PAYLOAD - hdr_sz;

    uint32_t frag_count = (enc_len + frag_cap - 1) / frag_cap;
    if (frag_count == 0) {
        frag_count = 1;
    }
    if (frag_count > FRAME_MAX_FRAGMENTS) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t off = 0;
    for (uint32_t i = 0; i < frag_count; i++) {
        uint32_t n = enc_len - off;
        if (n > frag_cap) {
            n = frag_cap;
        }

        uint8_t flags = 0;
        if (i == 0) {
            flags |= ESPGPU_FF_FIRST;
        }
        if (i + 1 == frag_count) {
            flags |= ESPGPU_FF_LAST;
        }

        espgpu_frame_hdr_t fh = {
            .frame_id = frame_id,
            .width = w,
            .height = h,
            .origin_x = 0,
            .origin_y = 0,
            .origin_w = w,
            .origin_h = h,
            .data_len = n,
            .raw_len = raw_len,
            .total_enc = enc_len,
            .format = format,
            .codec = codec,
            .frag_index = (uint8_t)i,
            .frag_count = (uint8_t)frag_count,
            .flags = flags,
            .palette_dirty = 0,
        };

        /* 超时给足：这里一旦超时就会 clear FIFO，把半截 Chunk 留在总线上，
         * 主机必然失步，然后变得更慢、更容易超时 —— 自我强化的死亡螺旋。
         * 32KB 在 1.2MB/s 下约 26ms，500ms 足够容忍主机的抖动。 */
        esp_err_t err = espgpu_usb_send_chunk_iov(ESPGPU_CHUNK_FRAME, 0, &fh, hdr_sz, enc + off,
                                                  n, 500);
        if (err != ESP_OK) {
            return err;
        }
        off += n;
    }
    return ESP_OK;
}

/* ── 呈现任务（测试图案） ─────────────────────────────────────────── */

void espgpu_gpu_present_task(void *arg)
{
    (void)arg;

    while (1) {
        int64_t t0 = esp_timer_get_time();

        xSemaphoreTake(g_gpu.lock, portMAX_DELAY);

        /* 控制任务挂上来的模式变更在这里落地 —— 只有呈现任务改 fb/mode，
         * 不会和发送路径抢锁。 */
        if (g_gpu.mode_pending) {
            apply_mode_locked(&g_gpu.pending_mode, g_gpu.pending_pattern);
            g_gpu.mode_pending = false;
        }

        const bool held =
            (int32_t)((uint32_t)(esp_timer_get_time() / 1000) - g_gpu.pattern_hold_until_ms) < 0;
        if (!g_gpu.streaming || g_gpu.command_mode || held || !espgpu_usb_ready() ||
            g_gpu.enc_buf == NULL) {
            xSemaphoreGive(g_gpu.lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 1. 生成内容 */
        espgpu_fb_test_pattern(&g_gpu.fb, g_gpu.frame_id, g_gpu.pattern);
        int64_t t1 = esp_timer_get_time();

        /* 模式刚变过就出一帧关键帧，让主机重建差分参考。
         * 请求在这里就取走；这一帧没发成功再放回去。 */
        uint8_t codec = g_gpu.mode.codec;
        const bool key_req = take_keyframe_request();
        if (codec == ESPGPU_CODEC_DELTA_RLE && key_req) {
            codec = ESPGPU_CODEC_RLE;
        }

        /* 2. 编码 */
        espgpu_fb_encode_result_t res;
        esp_err_t err = espgpu_fb_encode(&g_gpu.fb, codec, g_gpu.enc_buf, g_gpu.enc_cap, &res);
        int64_t t2 = esp_timer_get_time();

        if (err != ESP_OK) {
            g_gpu.stats.frames_dropped++;
            if (key_req) {
                g_gpu.force_keyframe = true;
            }
            xSemaphoreGive(g_gpu.lock);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 3. 发送 */
        err = send_frame(g_gpu.enc_buf, res.enc_bytes, res.raw_bytes, codec, g_gpu.fb.width,
                         g_gpu.fb.height, g_gpu.fb.format, g_gpu.frame_id);
        int64_t t3 = esp_timer_get_time();

        if (err == ESP_OK) {
            espgpu_fb_swap(&g_gpu.fb); /* 只有发成功才更新差分参考帧 */
            g_gpu.frame_id++;
            g_gpu.stats.frames_presented++;
        } else {
            /* 主机至多收到半帧（会丢弃）；参考帧没换，但保险起见下一帧走关键帧 */
            g_gpu.force_keyframe = true;
            g_gpu.stats.frames_dropped++;
        }

        g_gpu.stats.frame_raw_bytes = res.raw_bytes;
        g_gpu.stats.frame_enc_bytes = res.enc_bytes;
        g_gpu.stats.t_raster_us = (uint32_t)(t1 - t0);
        g_gpu.stats.t_encode_us = (uint32_t)(t2 - t1);
        g_gpu.stats.t_usb_us = (uint32_t)(t3 - t2);

        xSemaphoreGive(g_gpu.lock);

        /* 4. 帧率控制：按剩余预算休眠。
         * 即使落后于目标帧率也要让出至少 1ms —— 呈现任务整帧持有 g_gpu.lock，
         * 如果释放后立刻重新抢占，其他核上的控制任务会长时间拿不到锁。 */
        int64_t elapsed = esp_timer_get_time() - t0;
        int64_t remain = (int64_t)g_gpu.present_period_us - elapsed;
        if (remain > 2000) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(remain / 1000)));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

/* ── 命令模式：编码 + 交给发送任务 ───────────────────────────────── */

/** 把所有槽位从发送任务手里收回来（换缓冲前调用）。失败时已拿到的会还回去。 */
static bool own_all_slots(TickType_t wait)
{
    int taken[SEND_SLOTS];
    int n = 0;
    while (n < SEND_SLOTS && xQueueReceive(s_free_q, &taken[n], wait) == pdTRUE) {
        n++;
    }
    if (n < SEND_SLOTS) {
        for (int i = 0; i < n; i++) {
            xQueueSend(s_free_q, &taken[i], 0);
        }
        return false;
    }
    return true;
}

static void release_all_slots(void)
{
    for (int i = 0; i < SEND_SLOTS; i++) {
        xQueueSend(s_free_q, &i, 0);
    }
}

static bool ensure_slots(size_t raw)
{
    const size_t cap = raw + raw / 2 + 64; /* RLE 最坏远小于 1.5x，留足余量 */
    bool need = false;
    for (int i = 0; i < SEND_SLOTS; i++) {
        need |= s_slots[i].buf == NULL || s_slots[i].cap < cap;
    }
    if (need) {
        /* 发送任务可能正在读某个槽位：先全部收回再换，否则就是 use-after-free
         * （SET_MODE 的 drain_sender 最多等 1 秒，USB 卡住时它会放弃） */
        if (!own_all_slots(pdMS_TO_TICKS(2000))) {
            ESP_LOGW(TAG, "send slots busy, cannot resize");
            return false;
        }
        bool ok = true;
        for (int i = 0; i < SEND_SLOTS; i++) {
            if (s_slots[i].buf && s_slots[i].cap >= cap) {
                continue;
            }
            if (s_slots[i].buf) {
                heap_caps_free(s_slots[i].buf);
            }
            s_slots[i].buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
            s_slots[i].cap = s_slots[i].buf ? cap : 0;
            if (s_slots[i].buf == NULL) {
                ESP_LOGE(TAG, "send slot alloc failed (%u)", (unsigned)cap);
                ok = false;
            }
        }
        release_all_slots();
        if (!ok) {
            return false;
        }
    }
    if (g_gpu.shadow_bytes < raw) {
        if (g_gpu.shadow) {
            heap_caps_free(g_gpu.shadow);
        }
        g_gpu.shadow = heap_caps_malloc(raw, MALLOC_CAP_SPIRAM);
        g_gpu.shadow_bytes = g_gpu.shadow ? raw : 0;
        if (g_gpu.shadow) {
            memset(g_gpu.shadow, 0, raw);
        }
        g_gpu.force_keyframe = true;
    }
    return g_gpu.shadow != NULL;
}

/** 等所有已排队的帧发完（改分辨率前调用，避免释放正在发送的缓冲） */
static void drain_sender(void)
{
    for (int i = 0; i < 200 && uxQueueMessagesWaiting(s_free_q) < SEND_SLOTS; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/** BLOCK 编码一帧到槽位（shadow 同时更新）。块编码只支持 RGB565，命令模式本来就是。 */
static esp_err_t encode_block(const uint16_t *pixels, uint16_t w, uint16_t h, bool key,
                              send_slot_t *sl, espgpu_fb_encode_result_t *res)
{
    const size_t n = espgpu_block_encode(pixels, (uint16_t *)g_gpu.shadow, w, h, key ? 1 : 0,
                                         sl->buf, sl->cap);
    if (n == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    res->raw_bytes = (uint32_t)w * h * 2u;
    res->enc_bytes = (uint32_t)n;
    return ESP_OK;
}

/** AUTO 每帧带宽预算（字节）：链路速率 / 目标帧率，至少 8 KB */
static uint32_t auto_budget(void)
{
    const uint32_t fps = g_gpu.mode.max_fps ? g_gpu.mode.max_fps : 24u;
    const uint32_t b = CHAIN_AUTO_LINK_BPS / fps;
    return b < 8192u ? 8192u : b;
}

/**
 * 按 g_gpu.frame_codec 编码一帧，决定 codec 与是否关键帧。
 * 返回 ESP_OK 时 shadow == pixels（下一帧的差分参考）。
 */
static esp_err_t encode_frame(const uint16_t *pixels, uint16_t w, uint16_t h, size_t raw,
                              bool need_key, send_slot_t *sl, uint8_t *codec_out, bool *key_out,
                              espgpu_fb_encode_result_t *res)
{
    const uint8_t *cur = (const uint8_t *)pixels;
    chain_prod_t *p = &s_chain_prod;
    esp_err_t err;

    switch (g_gpu.frame_codec) {
    case ESPGPU_CODEC_RAW:
    case ESPGPU_CODEC_RLE:
        *codec_out = g_gpu.frame_codec;
        *key_out = true;
        return espgpu_encode_update(cur, g_gpu.shadow, raw, 2, g_gpu.frame_codec, sl->buf,
                                    sl->cap, res);

    case ESPGPU_CODEC_BLOCK:
        *codec_out = ESPGPU_CODEC_BLOCK;
        *key_out = need_key;
        return encode_block(pixels, w, h, need_key, sl, res);

    case ESPGPU_CODEC_AUTO: {
        const uint32_t budget = auto_budget();
        const uint32_t nblocks = espgpu_block_count(w, h);
        const uint32_t bitmap = espgpu_block_bitmap_bytes(w, h);
        if (p->refresh_backoff) {
            p->refresh_backoff--;
        }

        if (!p->lossy) {
            /* 主机画面是精确的：优先无损。放不进预算（镜头在动）就改发有损关键帧。
             * 无损编码已经把 shadow 更新成这一帧，块编码再对它做一遍是无害的拷贝。 */
            const uint8_t c = need_key ? ESPGPU_CODEC_RLE : ESPGPU_CODEC_DELTA_RLE;
            err = espgpu_encode_update(cur, g_gpu.shadow, raw, 2, c, sl->buf, sl->cap, res);
            /* 关键帧（周期性兜底 / 主机要求）允许超预算几倍：只是偶尔一帧慢一点，
             * 不值得为此把静止画面降成有损 */
            const uint32_t limit = need_key ? budget * CHAIN_AUTO_REFRESH_SLACK : budget;
            if (err == ESP_OK && res->enc_bytes <= limit) {
                *codec_out = c;
                *key_out = need_key;
                return ESP_OK;
            }
            p->calm = 0;
            *codec_out = ESPGPU_CODEC_BLOCK;
            *key_out = true;
            return encode_block(pixels, w, h, true, sl, res);
        }

        /* 有损状态：画面静止了一阵就补一帧无损关键帧，把有损误差清掉 */
        if (p->calm >= CHAIN_AUTO_CALM_FRAMES && p->refresh_backoff == 0) {
            p->calm = 0;
            err = espgpu_encode_update(cur, g_gpu.shadow, raw, 2, ESPGPU_CODEC_RLE, sl->buf,
                                       sl->cap, res);
            if (err == ESP_OK && res->enc_bytes <= budget * CHAIN_AUTO_REFRESH_SLACK) {
                *codec_out = ESPGPU_CODEC_RLE;
                *key_out = true;
                return ESP_OK;
            }
            /* 太大（纹理很碎）：这次放弃，冷却一段时间。shadow 已是这一帧，只能发块关键帧 */
            p->refresh_backoff = CHAIN_AUTO_REFRESH_BACKOFF;
            *codec_out = ESPGPU_CODEC_BLOCK;
            *key_out = true;
            return encode_block(pixels, w, h, true, sl, res);
        }

        *codec_out = ESPGPU_CODEC_BLOCK;
        *key_out = need_key;
        err = encode_block(pixels, w, h, need_key, sl, res);
        if (err == ESP_OK) {
            const uint32_t changed = (res->enc_bytes - bitmap) / 8u;
            p->calm = (!need_key && changed <= nblocks / CHAIN_AUTO_CALM_DIV) ? p->calm + 1u : 0u;
        }
        return err;
    }

    case ESPGPU_CODEC_DELTA_RLE:
    default: {
        const uint8_t c = chain_pick_delta_rle(p, need_key, raw);
        *codec_out = c;
        *key_out = c != ESPGPU_CODEC_DELTA_RLE;
        return espgpu_encode_update(cur, g_gpu.shadow, raw, 2, c, sl->buf, sl->cap, res);
    }
    }
}

/** core 的 PRESENT 回调：在命令任务上下文执行 */
static void on_core_present(void *user, const uint16_t *pixels, uint16_t w, uint16_t h)
{
    (void)user;
    const size_t raw = (size_t)w * h * 2;
    if (!ensure_slots(raw)) {
        g_gpu.stats.frames_dropped++;
        return;
    }

    /* 等一个空闲槽位：发送比渲染慢时在这里形成背压。
     * 超时丢帧不用关键帧：shadow 没动，主机参考帧仍然和它一致。 */
    int slot = -1;
    if (xQueueReceive(s_free_q, &slot, pdMS_TO_TICKS(500)) != pdTRUE) {
        g_gpu.stats.frames_dropped++;
        return;
    }
    send_slot_t *sl = &s_slots[slot];

    const bool key_req = take_keyframe_request();
    const bool need_key = chain_need_key(&s_chain_prod, &s_chain_tx, key_req);

    const int64_t t0 = esp_timer_get_time();
    uint8_t codec;
    bool key;
    espgpu_fb_encode_result_t res = {0};
    esp_err_t err = encode_frame(pixels, w, h, raw, need_key, sl, &codec, &key, &res);
    g_gpu.stats.t_encode_us = (uint32_t)(esp_timer_get_time() - t0);
    if (err != ESP_OK) {
        /* shadow 可能已被部分更新，必须重建参考帧 */
        g_gpu.force_keyframe = true;
        g_gpu.stats.frames_dropped++;
        xQueueSend(s_free_q, &slot, 0);
        return;
    }
    chain_produced(&s_chain_prod, key, codec, res.enc_bytes);

    sl->enc_len = res.enc_bytes;
    sl->raw_len = res.raw_bytes;
    sl->frame_id = g_gpu.frame_id++;
    sl->w = w;
    sl->h = h;
    sl->codec = codec;
    sl->key = key;
    g_gpu.stats.frame_raw_bytes = res.raw_bytes;
    g_gpu.stats.frame_enc_bytes = res.enc_bytes;
    xQueueSend(s_send_q, &slot, portMAX_DELAY);
}

void espgpu_gpu_sender_task(void *arg)
{
    (void)arg;
    uint32_t dbg_frames = 0;
    for (;;) {
        int slot;
        if (xQueueReceive(s_send_q, &slot, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        send_slot_t *sl = &s_slots[slot];
        if (!chain_tx_should_send(&s_chain_tx, sl->key)) {
            /* 断链后的差分帧：参考帧主机没有，发了只会花屏 */
            g_gpu.stats.frames_dropped++;
            xQueueSend(s_free_q, &slot, portMAX_DELAY);
            continue;
        }
        const int64_t t0 = esp_timer_get_time();
        esp_err_t err = send_frame(sl->buf, sl->enc_len, sl->raw_len, sl->codec, sl->w, sl->h,
                                   ESPGPU_FMT_RGB565, sl->frame_id);
        g_gpu.stats.t_usb_us = (uint32_t)(esp_timer_get_time() - t0);
        /* 先记结果再还槽位：命令任务拿到这个槽位时一定已经看得到 fail_seq */
        chain_tx_done(&s_chain_tx, sl->key, err == ESP_OK);
        if (err == ESP_OK) {
            g_gpu.stats.frames_presented++;
        } else {
            g_gpu.stats.frames_dropped++;
        }
        const uint32_t enc_len = sl->enc_len;
        const uint32_t raw_len = sl->raw_len;
        const uint8_t codec = sl->codec;
        const bool key = sl->key;
        xQueueSend(s_free_q, &slot, portMAX_DELAY);

        if (++dbg_frames % 30 == 0) {
            /* 分段 profile（issue #2）：读 core 的统计是无锁的整数快照，偶尔撕裂无所谓 */
            espgpu_stats_t cs = {0};
            gpu_core_fill_stats(g_gpu.core, &cs);
            ESP_LOGI(TAG,
                     "frame %u: raster %u us  tnl %u us  encode %u us  usb %u us  enc %u/%u B "
                     "codec %u%s",
                     (unsigned)dbg_frames, (unsigned)cs.t_raster_us, (unsigned)cs.t_tnl_us,
                     (unsigned)g_gpu.stats.t_encode_us, (unsigned)g_gpu.stats.t_usb_us,
                     (unsigned)enc_len, (unsigned)raw_len, (unsigned)codec, key ? " key" : "");
            ESP_LOGI(TAG,
                     "  stages: vtx %u clip %u setup %u bin %u tile %u tile_io %u us | "
                     "tris %u culled %u | paths fill %u fast %u px %u generic %u | passes %u",
                     (unsigned)cs.t_vtx_us, (unsigned)cs.t_clip_us, (unsigned)cs.t_setup_us,
                     (unsigned)cs.t_bin_us, (unsigned)cs.t_tile_us, (unsigned)cs.t_tile_io_us,
                     (unsigned)cs.triangles, (unsigned)cs.culled, (unsigned)cs.tris_fill,
                     (unsigned)cs.tris_fast, (unsigned)cs.tris_px, (unsigned)cs.tris_generic,
                     (unsigned)cs.raster_passes);
        }
    }
}

static void on_core_event(void *user, const espgpu_event_t *ev)
{
    (void)user;
    espgpu_usb_send_event(ev->kind, ev->code, ev->value, ev->arg, 200);
}

/* ── 时钟 ─────────────────────────────────────────────────────────── */

static esp_err_t set_cpu_clock(uint32_t mhz)
{
    if (mhz != 80 && mhz != 160 && mhz != 240) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = (int)mhz,
        .min_freq_mhz = (int)mhz, /* 锁频：DFS 切换会打断 USB 与光栅的节奏 */
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err == ESP_OK) {
        g_gpu.cpu_mhz = (uint16_t)mhz;
        ESP_LOGI(TAG, "CPU clock -> %u MHz", (unsigned)mhz);
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ── OTA ──────────────────────────────────────────────────────────── */

static uint16_t ota_begin(uint32_t total)
{
    if (s_ota.state == 1) {
        esp_ota_abort(s_ota.handle);
    }
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.part = esp_ota_get_next_update_partition(NULL);
    if (s_ota.part == NULL || total == 0 || total > s_ota.part->size) {
        s_ota.state = 3;
        return ESPGPU_ERR_INVALID_ARG;
    }
    /* 顺序写模式：边写边擦，不在 BEGIN 里一次擦 3 MB（那会让控制请求超时） */
    esp_err_t err = esp_ota_begin(s_ota.part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin: %s", esp_err_to_name(err));
        s_ota.state = 3;
        return ESPGPU_ERR_INTERNAL;
    }
    s_ota.total = total;
    s_ota.state = 1;
    ESP_LOGI(TAG, "OTA begin: %u bytes -> %s", (unsigned)total, s_ota.part->label);
    return ESPGPU_OK;
}

static uint16_t ota_data(uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (s_ota.state != 1) {
        return ESPGPU_ERR_BAD_STATE;
    }
    if (offset != s_ota.written || s_ota.written + len > s_ota.total) {
        return ESPGPU_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_ota_write(s_ota.handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota write @%u: %s", (unsigned)offset, esp_err_to_name(err));
        esp_ota_abort(s_ota.handle);
        s_ota.state = 3;
        return ESPGPU_ERR_INTERNAL;
    }
    s_ota.crc = esp_rom_crc32_le(s_ota.crc, data, len);
    s_ota.written += len;
    return ESPGPU_OK;
}

static uint16_t ota_end(uint32_t crc)
{
    if (s_ota.state != 1) {
        return ESPGPU_ERR_BAD_STATE;
    }
    if (s_ota.written != s_ota.total) {
        esp_ota_abort(s_ota.handle);
        s_ota.state = 3;
        return ESPGPU_ERR_INVALID_ARG;
    }
    if (crc != 0 && crc != s_ota.crc) {
        ESP_LOGE(TAG, "OTA crc mismatch: host %08x dev %08x", (unsigned)crc,
                 (unsigned)s_ota.crc);
        esp_ota_abort(s_ota.handle);
        s_ota.state = 3;
        return ESPGPU_ERR_CRC;
    }
    esp_err_t err = esp_ota_end(s_ota.handle);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(s_ota.part);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota end: %s", esp_err_to_name(err));
        s_ota.state = 3;
        return ESPGPU_ERR_INTERNAL;
    }
    s_ota.state = 2;
    ESP_LOGI(TAG, "OTA complete, reboot to activate %s", s_ota.part->label);
    return ESPGPU_OK;
}

/* ── 日志转发 ─────────────────────────────────────────────────────── */

static logfwd_ring_t s_log_ring;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_log_level;   /* 0 = 未订阅 */
static vprintf_like_t s_orig_vprintf;
static volatile TaskHandle_t s_log_drainer; /* 正在发日志的任务：它自己的日志不再入队 */

/* esp_log 的 vprintf 钩子：任意任务上下文（ESP_LOGx 不允许在 ISR 里用；ISR / 早期日志
 * 走 esp_rom_printf，不经过这里）。原样入队，判级别 / 分行 / 去颜色都在 drain_logs。 */
static int log_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    const int r = s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);
    if (s_log_level > 0 && s_log_drainer != xTaskGetCurrentTaskHandle()) {
        char line[200];
        int n = vsnprintf(line, sizeof(line), fmt, ap2);
        if (n > (int)sizeof(line) - 1) {
            /* 截断了：如果原本以换行结尾，保住换行，否则会和下一行粘在一起 */
            n = (int)sizeof(line) - 1;
            const size_t fl = strlen(fmt);
            if (fl && fmt[fl - 1] == '\n') {
                line[n - 1] = '\n';
            }
        }
        if (n > 0) {
            portENTER_CRITICAL(&s_log_mux);
            logfwd_push(&s_log_ring, line, (uint32_t)n); /* 满了整段丢弃 */
            portEXIT_CRITICAL(&s_log_mux);
        }
    }
    va_end(ap2);
    return r;
}

void espgpu_log_forward_init(void)
{
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
}

/** 在命令任务里调用：把积压的完整日志行逐行发出 */
static void drain_logs(void)
{
    if (s_log_level <= 0 || logfwd_used(&s_log_ring) == 0 || !espgpu_usb_ready()) {
        return;
    }
    char buf[512];
    portENTER_CRITICAL(&s_log_mux);
    const uint32_t n = logfwd_peek(&s_log_ring, buf, sizeof(buf));
    portEXIT_CRITICAL(&s_log_mux);

    /* 只发到最后一个换行；半行留在环里等下次（单行超过 buf 才强制切断） */
    const uint32_t take = logfwd_complete_prefix(buf, n, sizeof(buf));
    if (take == 0) {
        return;
    }
    portENTER_CRITICAL(&s_log_mux);
    logfwd_consume(&s_log_ring, take); /* 只有这里推进 tail，peek 之后不会变 */
    portEXIT_CRITICAL(&s_log_mux);

    s_log_drainer = xTaskGetCurrentTaskHandle();
    char line[512];
    size_t start = 0;
    for (size_t i = 0; i <= take; i++) {
        if (i < take && buf[i] != '\n') {
            continue;
        }
        const size_t ln = logfwd_clean(line, buf + start, i - start);
        start = i + 1;
        if (ln && logfwd_level(line, ln) <= s_log_level) {
            espgpu_usb_send_chunk(ESPGPU_CHUNK_LOG, 0, line, (uint32_t)ln, 20);
        }
    }
    s_log_drainer = NULL;
}

/* ── 控制请求 ─────────────────────────────────────────────────────── */

/**
 * OTA 回滚确认：新固件要等主机真的通过 USB 和我们说上话才算自检通过。
 * 以前在 app_main 里 USB 起来之前就确认了 —— USB 坏掉的固件恰恰是最需要回滚的。
 * 没有主机连上就重启（或掉电）会回滚到旧固件，这是有意的。
 */
static void confirm_running_app_once(void)
{
    static bool s_done;
    if (s_done) {
        return;
    }
    s_done = true;
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed by host: %s", esp_err_to_name(err));
    }
}

/**
 * 回到测试图案模式（issue #4）：以前 command_mode 一旦置位只有复位才清，跑过任何
 * GL / demo 之后面板和 bench 就再也看不到帧。主机侧 WinUSB 是独占的，换一个程序打开设备
 * （握手 GET_DEVICE_INFO）或者改模式（SET_MODE）都说明之前的命令流已经结束；
 * 如果新程序接着发命令，handle_cmd_chunk 会重新进入命令模式并强制关键帧。
 */
static void leave_command_mode(void)
{
    if (!g_gpu.command_mode) {
        return;
    }
    g_gpu.pattern_hold_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 250u;
    g_gpu.command_mode = false;
    /* 测试图案路径的差分参考（fb 双缓冲）主机早就没有了，第一帧必须是关键帧 */
    g_gpu.force_keyframe = true;
    ESP_LOGI(TAG, "leaving command mode, test patterns resume");
}

static void handle_ctrl(const espgpu_ctrl_req_t *req, const uint8_t *extra, uint32_t extra_len)
{
    uint16_t status = ESPGPU_OK;
    ESP_LOGD(TAG, "ctrl op=0x%04X seq=%u arg0=%u len=%u", req->op, req->seq,
             (unsigned)req->arg0, (unsigned)extra_len);

    switch (req->op) {
    case ESPGPU_CTRL_PING:
        espgpu_usb_send_ctrl_resp(req->op, req->seq, ESPGPU_OK, req, sizeof(*req), 200);
        return;

    case ESPGPU_CTRL_GET_DEVICE_INFO: {
        leave_command_mode(); /* espgpu_open() 的握手：新的主机程序接管了设备 */
        espgpu_device_info_t di;
        fill_device_info(&di);
        espgpu_usb_send_ctrl_resp(req->op, req->seq, ESPGPU_OK, &di, sizeof(di), 500);
        return;
    }

    case ESPGPU_CTRL_GET_STATS: {
        espgpu_gpu_sample_stats();
        espgpu_stats_t st = g_gpu.stats;
        if (g_gpu.core && g_gpu.command_mode) {
            gpu_core_fill_stats(g_gpu.core, &st);
        }
        espgpu_usb_send_ctrl_resp(req->op, req->seq, ESPGPU_OK, &st, sizeof(st), 500);
        return;
    }

    case ESPGPU_CTRL_SET_MODE: {
        espgpu_mode_t m = g_gpu.mode;
        if (extra_len >= sizeof(m)) {
            memcpy(&m, extra, sizeof(m));
        }
        /* 命令模式支持 BLOCK / AUTO；测试图案路径（sanitize_mode）只支持到 DELTA_RLE */
        const uint8_t req_codec = m.codec;
        m = sanitize_mode(&m);
        /* 命令模式的屏幕目标由命令任务（也就是这里）拥有，直接改 */
        if (g_gpu.core) {
            drain_sender();
            if (!gpu_core_set_size(g_gpu.core, m.width, m.height)) {
                status = ESPGPU_ERR_NO_MEM;
            }
            g_gpu.force_keyframe = true;
        }
        if (req_codec <= ESPGPU_CODEC_AUTO && req_codec != ESPGPU_CODEC_TILE_DELTA) {
            g_gpu.frame_codec = req_codec;
        }
        esp_err_t err = espgpu_gpu_set_mode(&m, (uint8_t)req->arg0);
        if (err != ESP_OK && status == ESPGPU_OK) {
            status = ESPGPU_ERR_NO_MEM;
        }
        /* gpu_core_set_size 已经丢掉了未完成的分箱；测试图案在短暂延迟后恢复 */
        leave_command_mode();
        break;
    }

    case ESPGPU_CTRL_SET_CLUT:
        if (extra_len >= ESPGPU_CLUT_ENTRIES * sizeof(uint16_t)) {
            espgpu_gpu_set_clut((const uint16_t *)extra);
        } else {
            status = ESPGPU_ERR_INVALID_ARG;
        }
        break;

    case ESPGPU_CTRL_RESET_STATS:
        memset(&g_gpu.stats, 0, sizeof(g_gpu.stats));
        s_boot_us = esp_timer_get_time();
        break;

    case ESPGPU_CTRL_SET_CLOCK: {
        esp_err_t err = set_cpu_clock(req->arg0);
        status = err == ESP_OK ? ESPGPU_OK
                 : err == ESP_ERR_INVALID_ARG ? ESPGPU_ERR_INVALID_ARG
                                              : ESPGPU_ERR_UNSUPPORTED;
        break;
    }

    case ESPGPU_CTRL_OTA_BEGIN:
        status = ota_begin(req->arg0);
        break;
    case ESPGPU_CTRL_OTA_DATA:
        status = ota_data(req->arg0, extra, extra_len);
        break;
    case ESPGPU_CTRL_OTA_END:
        status = ota_end(req->arg0);
        break;
    case ESPGPU_CTRL_OTA_ABORT:
        if (s_ota.state == 1) {
            esp_ota_abort(s_ota.handle);
        }
        memset(&s_ota, 0, sizeof(s_ota));
        break;

    case ESPGPU_CTRL_LOG_SUBSCRIBE:
        /* 丢掉上次订阅留下的积压，从现在开始转发 */
        portENTER_CRITICAL(&s_log_mux);
        s_log_ring.tail = s_log_ring.head;
        portEXIT_CRITICAL(&s_log_mux);
        s_log_level = (int)(req->arg0 > 5 ? 5 : req->arg0);
        break;

    case ESPGPU_CTRL_SOFT_RESET:
        espgpu_usb_send_ctrl_resp(req->op, req->seq, ESPGPU_OK, NULL, 0, 500);
        ESP_LOGW(TAG, "soft reset requested by host");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;

    case ESPGPU_CTRL_REQUEST_KEYFRAME:
        /* 主机解码失败时用这个把两边重新对齐 */
        g_gpu.force_keyframe = true;
        break;

    case ESPGPU_CTRL_SET_DEBUG_VIEW:
        if (g_gpu.core) {
            gpu_core_set_debug_view(g_gpu.core, (uint8_t)req->arg0);
        }
        break;

    case ESPGPU_CTRL_SET_CODEC:
        if (req->arg0 <= ESPGPU_CODEC_AUTO && req->arg0 != ESPGPU_CODEC_TILE_DELTA) {
            g_gpu.frame_codec = (uint8_t)req->arg0;
            g_gpu.force_keyframe = true;
        } else {
            status = ESPGPU_ERR_UNSUPPORTED;
        }
        break;

    default:
        status = ESPGPU_ERR_UNSUPPORTED;
        break;
    }

    ESP_LOGD(TAG, "ctrl op=0x%04X -> status=%u", req->op, status);
    espgpu_usb_send_ctrl_resp(req->op, req->seq, status, NULL, 0, 500);
}

/* ── 命令 / 数据 chunk ───────────────────────────────────────────── */

/** 带 ESPGPU_F_LZ 时解压，返回可用载荷（失败返回 NULL） */
static const uint8_t *unpack(const espgpu_chunk_hdr_t *hdr, const uint8_t *payload,
                             uint32_t *len)
{
    if (!(hdr->flags & ESPGPU_F_LZ)) {
        *len = hdr->length;
        return payload;
    }
    if (hdr->length < 4) {
        return NULL;
    }
    uint32_t raw;
    memcpy(&raw, payload, 4);
    if (raw > LZ_BUF_SIZE) {
        return NULL;
    }
    if (espgpu_lz_decode(payload + 4, hdr->length - 4, s_lz_buf, raw) != raw) {
        ESP_LOGW(TAG, "LZ chunk corrupt (%u -> %u)", hdr->length, (unsigned)raw);
        return NULL;
    }
    g_gpu.stats.lz_in_bytes += hdr->length;
    g_gpu.stats.lz_out_bytes += raw;
    *len = raw;
    return s_lz_buf;
}

static void handle_cmd_chunk(const uint8_t *data, uint32_t len)
{
    /* 收到任何 GPU 命令就认为主机接管了渲染，停掉 M1 的测试图案自动出帧。
     * 切换瞬间必须强制关键帧：设备的 shadow 是旧内容，主机参考帧是测试图案。 */
    if (!g_gpu.command_mode) {
        g_gpu.command_mode = true;
        /* 等呈现任务退出当前这一帧，避免两条路径同时发帧 */
        xSemaphoreTake(g_gpu.lock, portMAX_DELAY);
        xSemaphoreGive(g_gpu.lock);
        /* 必须在握手之后置位：呈现任务那一帧发成功时会清掉之前的请求，
         * 第一帧命令模式画面就会对着主机的测试图案参考帧做差分 */
        g_gpu.force_keyframe = true;
    }

    gpu_core_exec(g_gpu.core, data, len);
    g_gpu.stats.cmd_count++;
}

/** 必须在命令任务里执行的控制请求：会改屏幕目标 / 核心状态 / 发送槽位 */
static bool ctrl_needs_cmd_task(uint16_t op)
{
    return op == ESPGPU_CTRL_SET_MODE || op == ESPGPU_CTRL_SET_DEBUG_VIEW;
}

void espgpu_gpu_rx_task(void *arg)
{
    (void)arg;
    int b = -1;

    while (1) {
        if (b < 0 && xQueueReceive(s_rx_free_q, &b, pdMS_TO_TICKS(100)) != pdTRUE) {
            /* 命令任务手上压着所有缓冲（主机提交得比设备快）：不读 USB 就是背压 */
            b = -1;
            continue;
        }

        rx_item_t it;
        esp_err_t err =
            espgpu_usb_read_chunk(&it.hdr, s_rx_buf[b], ESPGPU_CHUNK_MAX_PAYLOAD, 100);
        if (err == ESP_ERR_TIMEOUT) {
            espgpu_gpu_sample_stats();
            continue;
        }
        if (err != ESP_OK) {
            g_gpu.stats.host_errors++;
            continue;
        }

        switch (it.hdr.type) {
        case ESPGPU_CHUNK_CTRL: {
            if (it.hdr.length < sizeof(espgpu_ctrl_req_t)) {
                g_gpu.stats.host_errors++;
                break;
            }
            espgpu_ctrl_req_t req;
            memcpy(&req, s_rx_buf[b], sizeof(req));
            if (ctrl_needs_cmd_task(req.op)) {
                it.kind = RX_CTRL;
                it.buf = (uint8_t)b;
                xQueueSend(s_rx_work_q, &it, portMAX_DELAY);
                b = -1;
            } else {
                handle_ctrl(&req, s_rx_buf[b] + sizeof(req), it.hdr.length - sizeof(req));
                confirm_running_app_once();
            }
            break;
        }
        case ESPGPU_CHUNK_CMD:
        case ESPGPU_CHUNK_DATA:
            it.kind = it.hdr.type == ESPGPU_CHUNK_CMD ? RX_CMD : RX_DATA;
            it.buf = (uint8_t)b;
            xQueueSend(s_rx_work_q, &it, portMAX_DELAY);
            b = -1;
            break;
        default:
            g_gpu.stats.host_errors++;
            break;
        }
    }
}

void espgpu_gpu_command_task(void *arg)
{
    (void)arg;

    while (1) {
        drain_logs();

        rx_item_t it;
        if (xQueueReceive(s_rx_work_q, &it, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        uint8_t *payload = s_rx_buf[it.buf];

        switch (it.kind) {
        case RX_CTRL: {
            espgpu_ctrl_req_t req;
            memcpy(&req, payload, sizeof(req));
            handle_ctrl(&req, payload + sizeof(req), it.hdr.length - sizeof(req));
            confirm_running_app_once();
            break;
        }
        case RX_CMD: {
            uint32_t len = 0;
            const uint8_t *p = unpack(&it.hdr, payload, &len);
            if (p) {
                handle_cmd_chunk(p, len);
            } else {
                g_gpu.stats.host_errors++;
            }
            break;
        }
        case RX_DATA: {
            uint32_t len = 0;
            const uint8_t *p = unpack(&it.hdr, payload, &len);
            if (p) {
                gpu_core_data(g_gpu.core, p, len);
            } else {
                g_gpu.stats.host_errors++;
            }
            break;
        }
        default:
            break;
        }

        const int b = it.buf;
        xQueueSend(s_rx_free_q, &b, portMAX_DELAY);
    }
}

/* ── 初始化 ───────────────────────────────────────────────────────── */

esp_err_t espgpu_gpu_init(void)
{
    memset(&g_gpu, 0, sizeof(g_gpu));
    s_boot_us = esp_timer_get_time();
    g_gpu.cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    g_gpu.lock = xSemaphoreCreateMutex();
    if (g_gpu.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_rx_free_q = xQueueCreate(RX_POOL, sizeof(int));
    s_rx_work_q = xQueueCreate(RX_POOL, sizeof(rx_item_t));
    if (s_rx_free_q == NULL || s_rx_work_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < RX_POOL; i++) {
        s_rx_buf[i] = heap_caps_malloc(ESPGPU_CHUNK_MAX_PAYLOAD, MALLOC_CAP_SPIRAM);
        if (s_rx_buf[i] == NULL) {
            ESP_LOGE(TAG, "rx buffer alloc failed");
            return ESP_ERR_NO_MEM;
        }
        xQueueSend(s_rx_free_q, &i, 0);
    }
    s_lz_buf = heap_caps_malloc(LZ_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (s_lz_buf == NULL) {
        ESP_LOGE(TAG, "command buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    espgpu_fb_default_clut(g_gpu.clut);

    esp_err_t err = espgpu_fb_init(&g_gpu.fb, 320, 200, ESPGPU_FMT_RGB565);
    if (err != ESP_OK) {
        return err;
    }
    err = realloc_encode_buffer();
    if (err != ESP_OK) {
        return err;
    }

    /* 设备核心：屏幕目标优先放内部 SRAM（光栅直写，免 tile 拷贝） */
    const gpu_core_config_t cfg = {
        .arena_bytes = ARENA_BYTES,
        .texture_budget = TEXTURE_BUDGET,
        .buffer_budget = BUFFER_BUDGET,
        .max_texture_dim = 512,
    };
    const gpu_core_cb_t cb = {
        .send_event = on_core_event,
        .present = on_core_present,
        .user = NULL,
    };
    g_gpu.core = gpu_core_create(&cfg, &cb);
    if (g_gpu.core == NULL) {
        ESP_LOGE(TAG, "gpu core init failed");
        return ESP_ERR_NO_MEM;
    }

    s_free_q = xQueueCreate(SEND_SLOTS, sizeof(int));
    s_send_q = xQueueCreate(SEND_SLOTS, sizeof(int));
    if (s_free_q == NULL || s_send_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < SEND_SLOTS; i++) {
        xQueueSend(s_free_q, &i, 0);
    }
    if (!ensure_slots((size_t)320 * 200 * 2)) {
        return ESP_ERR_NO_MEM;
    }

    g_gpu.frame_codec = ESPGPU_CODEC_DELTA_RLE;
    g_gpu.command_mode = false;
    g_gpu.frame_id = 1; /* 0 留给主机表示"还没有帧" */

    g_gpu.mode.width = g_gpu.fb.width;
    g_gpu.mode.height = g_gpu.fb.height;
    g_gpu.mode.format = g_gpu.fb.format;
    g_gpu.mode.codec = ESPGPU_CODEC_DELTA_RLE;
    g_gpu.mode.max_fps = 24;
    g_gpu.mode.vsync = 1;
    g_gpu.present_period_us = 1000000u / 24;
    g_gpu.pattern = ESPGPU_PATTERN_DOOMISH;
    g_gpu.streaming = true;
    g_gpu.force_keyframe = true;

#if CONFIG_PM_ENABLE
    set_cpu_clock(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#endif

    /* 内部温度传感器：范围按 S3 的 -10..80 度设定 */
    const temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    } else {
        s_tsens = NULL;
    }

    ESP_LOGI(TAG, "GPU init done: %ux%u, codec=delta_rle, target=24fps", g_gpu.fb.width,
             g_gpu.fb.height);
    return ESP_OK;
}
