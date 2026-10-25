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

#include "espgpu_lz.h"
#include "espgpu_proto.h"
#include "gpu.h"
#include "usb_device.h"
#include "version.h"

static const char *TAG = "gpu";

espgpu_gpu_t g_gpu;

static uint8_t *s_cmd_buf;              /* 接收命令的载荷缓冲（PSRAM） */
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
} send_slot_t;

#define SEND_SLOTS 2
static send_slot_t s_slots[SEND_SLOTS];
static QueueHandle_t s_free_q; /* 可写槽位下标 */
static QueueHandle_t s_send_q; /* 待发槽位下标 */

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
    di->codecs = ESPGPU_CAP_CODEC_RAW | ESPGPU_CAP_CODEC_RLE | ESPGPU_CAP_CODEC_DELTA_RLE;

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
    if (frag_count > ESPGPU_MAX_FRAGMENTS) {
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

        if (!g_gpu.streaming || g_gpu.command_mode || !espgpu_usb_ready() ||
            g_gpu.enc_buf == NULL) {
            xSemaphoreGive(g_gpu.lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 1. 生成内容 */
        espgpu_fb_test_pattern(&g_gpu.fb, g_gpu.frame_id, g_gpu.pattern);
        int64_t t1 = esp_timer_get_time();

        /* 模式刚变过就出一帧关键帧，让主机重建差分参考 */
        uint8_t codec = g_gpu.mode.codec;
        if (codec == ESPGPU_CODEC_DELTA_RLE && g_gpu.force_keyframe) {
            codec = ESPGPU_CODEC_RLE;
        }

        /* 2. 编码 */
        espgpu_fb_encode_result_t res;
        esp_err_t err = espgpu_fb_encode(&g_gpu.fb, codec, g_gpu.enc_buf, g_gpu.enc_cap, &res);
        int64_t t2 = esp_timer_get_time();

        if (err != ESP_OK) {
            g_gpu.stats.frames_dropped++;
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
            g_gpu.force_keyframe = false;
            g_gpu.frame_id++;
            g_gpu.stats.frames_presented++;
        } else {
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

static bool ensure_slots(size_t raw)
{
    const size_t cap = raw + raw / 2 + 64; /* RLE 最坏 (bpp+1)/bpp = 1.5x */
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

/** core 的 PRESENT 回调：在命令任务上下文执行 */
static void on_core_present(void *user, const uint16_t *pixels, uint16_t w, uint16_t h)
{
    (void)user;
    const size_t raw = (size_t)w * h * 2;
    if (!ensure_slots(raw)) {
        g_gpu.stats.frames_dropped++;
        return;
    }

    /* 等一个空闲槽位：发送比渲染慢时在这里形成背压 */
    int slot = -1;
    if (xQueueReceive(s_free_q, &slot, pdMS_TO_TICKS(500)) != pdTRUE) {
        g_gpu.stats.frames_dropped++;
        g_gpu.force_keyframe = true; /* 这一帧没发，主机参考帧仍是更早的帧 */
        return;
    }
    send_slot_t *sl = &s_slots[slot];

    uint8_t codec = g_gpu.frame_codec;
    if (codec == ESPGPU_CODEC_DELTA_RLE) {
        /* 关键帧：模式切换 / 发送失败 / 周期性兜底 / 上一帧差分效果很差（内容整屏在变，
         * 这时 RLE 更小 —— bench-results 里 bars 图案 10.2x vs 5.3x 的结论） */
        if (g_gpu.force_keyframe || g_gpu.frames_since_key >= 60 ||
            g_gpu.last_delta_bytes > raw / 3) {
            codec = ESPGPU_CODEC_RLE;
        }
    }

    /* 关键：只有 DELTA_RLE 才传参考帧。关键帧如果也传了 shadow，
     * 编码端做了异或而解码端没有，解出来必然是垃圾。 */
    const uint8_t *ref = (codec == ESPGPU_CODEC_DELTA_RLE) ? g_gpu.shadow : NULL;
    espgpu_fb_encode_result_t res;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = espgpu_encode_raw((const uint8_t *)pixels, ref, raw, 2, codec, sl->buf,
                                      sl->cap, &res);
    g_gpu.stats.t_encode_us = (uint32_t)(esp_timer_get_time() - t0);
    if (err != ESP_OK) {
        g_gpu.stats.frames_dropped++;
        xQueueSend(s_free_q, &slot, 0);
        return;
    }

    /* 乐观地更新参考帧：发送失败时发送任务会置 force_keyframe */
    memcpy(g_gpu.shadow, pixels, raw);
    if (codec == ESPGPU_CODEC_DELTA_RLE) {
        g_gpu.last_delta_bytes = res.enc_bytes;
        g_gpu.frames_since_key++;
    } else {
        g_gpu.last_delta_bytes = 0;
        g_gpu.frames_since_key = 0;
    }
    g_gpu.force_keyframe = false;

    sl->enc_len = res.enc_bytes;
    sl->raw_len = res.raw_bytes;
    sl->frame_id = g_gpu.frame_id++;
    sl->w = w;
    sl->h = h;
    sl->codec = codec;
    g_gpu.stats.frame_raw_bytes = res.raw_bytes;
    g_gpu.stats.frame_enc_bytes = res.enc_bytes;
    xQueueSend(s_send_q, &slot, portMAX_DELAY);
}

static uint32_t st_raster(void)
{
    espgpu_stats_t s = {0};
    gpu_core_fill_stats(g_gpu.core, &s);
    return s.t_raster_us;
}

static uint32_t st_tnl(void)
{
    espgpu_stats_t s = {0};
    gpu_core_fill_stats(g_gpu.core, &s);
    return s.t_tnl_us;
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
        const int64_t t0 = esp_timer_get_time();
        esp_err_t err = send_frame(sl->buf, sl->enc_len, sl->raw_len, sl->codec, sl->w, sl->h,
                                   ESPGPU_FMT_RGB565, sl->frame_id);
        g_gpu.stats.t_usb_us = (uint32_t)(esp_timer_get_time() - t0);
        if (err == ESP_OK) {
            g_gpu.stats.frames_presented++;
        } else {
            g_gpu.stats.frames_dropped++;
            g_gpu.force_keyframe = true;
        }
        xQueueSend(s_free_q, &slot, portMAX_DELAY);

        if (++dbg_frames % 30 == 0) {
            ESP_LOGI(TAG, "frame %u: raster %u us  tnl %u us  encode %u us  usb %u us  enc %u/%u B",
                     (unsigned)dbg_frames, (unsigned)st_raster(), (unsigned)st_tnl(), (unsigned)g_gpu.stats.t_encode_us,
                     (unsigned)g_gpu.stats.t_usb_us, (unsigned)sl->enc_len,
                     (unsigned)sl->raw_len);
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

#define LOG_RING 4096
static char s_log_ring[LOG_RING];
static volatile uint32_t s_log_head, s_log_tail;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_log_level;   /* 0 = 未订阅 */
static vprintf_like_t s_orig_vprintf;
static volatile bool s_log_sending;

static int level_of_line(const char *s)
{
    /* esp_log 行首：可能有 ANSI 颜色前缀 "\033[0;31m"，然后是 E/W/I/D/V */
    if (s[0] == '\033') {
        const char *m = strchr(s, 'm');
        if (m) s = m + 1;
    }
    switch (s[0]) {
    case 'E': return 1;
    case 'W': return 2;
    case 'I': return 3;
    case 'D': return 4;
    case 'V': return 5;
    default: return 3;
    }
}

static int log_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    const int r = s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);
    if (s_log_level > 0 && !s_log_sending) {
        char line[200];
        int n = vsnprintf(line, sizeof(line), fmt, ap2);
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        if (n > 0 && level_of_line(line) <= s_log_level) {
            portENTER_CRITICAL(&s_log_mux);
            for (int i = 0; i < n; i++) {
                const uint32_t next = (s_log_head + 1) % LOG_RING;
                if (next == s_log_tail) {
                    break; /* 满了丢弃 */
                }
                s_log_ring[s_log_head] = line[i];
                s_log_head = next;
            }
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

/** 在命令任务里调用：把积压的日志按行打包发出 */
static void drain_logs(void)
{
    if (s_log_level <= 0 || s_log_head == s_log_tail || !espgpu_usb_ready()) {
        return;
    }
    char buf[512];
    size_t n = 0;
    portENTER_CRITICAL(&s_log_mux);
    while (s_log_tail != s_log_head && n < sizeof(buf)) {
        buf[n++] = s_log_ring[s_log_tail];
        s_log_tail = (s_log_tail + 1) % LOG_RING;
    }
    portEXIT_CRITICAL(&s_log_mux);

    /* 按行发，去掉 ANSI 颜色序列（ESC [ ... m）和回车换行 */
    s_log_sending = true;
    char line[512];
    size_t ln = 0;
    for (size_t i = 0; i < n; i++) {
        const char ch = buf[i];
        if (ch == '\033') {
            while (i < n && buf[i] != 'm') {
                i++;
            }
            continue;
        }
        if (ch == '\n') {
            if (ln) {
                espgpu_usb_send_chunk(ESPGPU_CHUNK_LOG, 0, line, (uint32_t)ln, 20);
            }
            ln = 0;
            continue;
        }
        if (ch != '\r' && ln < sizeof(line)) {
            line[ln++] = ch;
        }
    }
    if (ln) {
        espgpu_usb_send_chunk(ESPGPU_CHUNK_LOG, 0, line, (uint32_t)ln, 20);
    }
    s_log_sending = false;
}

/* ── 控制请求 ─────────────────────────────────────────────────────── */

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
        m = sanitize_mode(&m);
        /* 命令模式的屏幕目标由命令任务（也就是这里）拥有，直接改 */
        if (g_gpu.core) {
            drain_sender();
            if (!gpu_core_set_size(g_gpu.core, m.width, m.height)) {
                status = ESPGPU_ERR_NO_MEM;
            }
            g_gpu.force_keyframe = true;
        }
        if (m.codec <= ESPGPU_CODEC_DELTA_RLE) {
            g_gpu.frame_codec = m.codec;
        }
        esp_err_t err = espgpu_gpu_set_mode(&m, (uint8_t)req->arg0);
        if (err != ESP_OK && status == ESPGPU_OK) {
            status = ESPGPU_ERR_NO_MEM;
        }
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
        if (req->arg0 <= ESPGPU_CODEC_DELTA_RLE) {
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
        g_gpu.force_keyframe = true;
        g_gpu.command_mode = true;
        /* 等呈现任务退出当前这一帧，避免两条路径同时发帧 */
        xSemaphoreTake(g_gpu.lock, portMAX_DELAY);
        xSemaphoreGive(g_gpu.lock);
    }

    gpu_core_exec(g_gpu.core, data, len);
    g_gpu.stats.cmd_count++;
}

void espgpu_gpu_command_task(void *arg)
{
    (void)arg;

    espgpu_chunk_hdr_t hdr;

    while (1) {
        drain_logs();

        esp_err_t err =
            espgpu_usb_read_chunk(&hdr, s_cmd_buf, ESPGPU_CHUNK_MAX_PAYLOAD, 100);
        if (err == ESP_ERR_TIMEOUT) {
            espgpu_gpu_sample_stats();
            continue;
        }
        if (err != ESP_OK) {
            g_gpu.stats.host_errors++;
            continue;
        }

        switch (hdr.type) {
        case ESPGPU_CHUNK_CTRL:
            if (hdr.length >= sizeof(espgpu_ctrl_req_t)) {
                espgpu_ctrl_req_t req;
                memcpy(&req, s_cmd_buf, sizeof(req));
                handle_ctrl(&req, s_cmd_buf + sizeof(req), hdr.length - sizeof(req));
            } else {
                g_gpu.stats.host_errors++;
            }
            break;

        case ESPGPU_CHUNK_CMD: {
            uint32_t len = 0;
            const uint8_t *p = unpack(&hdr, s_cmd_buf, &len);
            if (p) {
                handle_cmd_chunk(p, len);
            } else {
                g_gpu.stats.host_errors++;
            }
            break;
        }

        case ESPGPU_CHUNK_DATA: {
            uint32_t len = 0;
            const uint8_t *p = unpack(&hdr, s_cmd_buf, &len);
            if (p) {
                gpu_core_data(g_gpu.core, p, len);
            } else {
                g_gpu.stats.host_errors++;
            }
            break;
        }

        default:
            g_gpu.stats.host_errors++;
            break;
        }
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

    s_cmd_buf = heap_caps_malloc(ESPGPU_CHUNK_MAX_PAYLOAD, MALLOC_CAP_SPIRAM);
    s_lz_buf = heap_caps_malloc(LZ_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (s_cmd_buf == NULL || s_lz_buf == NULL) {
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
