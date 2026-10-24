/*
 * gpu.c — 设备状态、呈现循环、命令分发
 *
 * M1 阶段还没有真正的光栅器，呈现路径是：
 *   生成测试图案 -> 编码 -> 分片打成 FRAME Chunk -> 发给主机
 *
 * 这样能立刻把整条链路的吞吐、延迟、压缩率测出来，
 * 为 M2 的光栅器确定性能预算。命令/控制通路同样是最终形态，
 * M2 只需往 ESPGPU_CHUNK_CMD 的分发里加 opcode。
 */

#include <string.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"

#include "espgpu_proto.h"
#include "gpu.h"
#include "usb_device.h"
#include "version.h"

static const char *TAG = "gpu";

espgpu_gpu_t g_gpu;

static uint8_t *s_cmd_buf;              /* 接收命令的载荷缓冲（PSRAM） */
static temperature_sensor_handle_t s_tsens;
static int64_t s_boot_us;

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

    /* 上限按 PSRAM 与带宽共同决定：800x600 是"能分配"的上限，
     * 实际可用分辨率受 USB 上行带宽约束，见 PLAN.md 的带宽账本。 */
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

    di->capabilities = ESPGPU_CAP_INDEX8 | ESPGPU_CAP_RGB565 | ESPGPU_CAP_DEPTH |
                       ESPGPU_CAP_QUADS | ESPGPU_CAP_OTA;
    di->codecs = ESPGPU_CAP_CODEC_RAW | ESPGPU_CAP_CODEC_RLE | ESPGPU_CAP_CODEC_DELTA_RLE;

    di->texture_cache_size = 4u * 1024 * 1024;
    di->max_texture_dim = 512;
    di->max_texture_units = ESPGPU_MAX_TEXTURE_UNITS;
    di->max_vertex_attribs = ESPGPU_MAX_VERTEX_ATTRIBS;
    di->cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
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

/* 调用者必须持有 g_gpu.lock。 */
static esp_err_t apply_mode_locked(const espgpu_mode_t *mode, uint8_t pattern)
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

    /* 工作表面与差分参考帧也要跟着改尺寸 */
    if (g_gpu.surface.width != m.width || g_gpu.surface.height != m.height) {
        raster_surface_deinit(&g_gpu.surface);
        if (!raster_surface_init(&g_gpu.surface, m.width, m.height)) {
            return ESP_ERR_NO_MEM;
        }
        const size_t bytes = (size_t)m.width * m.height * 2;
        if (g_gpu.shadow_bytes < bytes) {
            if (g_gpu.shadow) {
                heap_caps_free(g_gpu.shadow);
            }
            g_gpu.shadow = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
            g_gpu.shadow_bytes = (g_gpu.shadow != NULL) ? bytes : 0;
        }
        if (g_gpu.shadow) {
            memset(g_gpu.shadow, 0, bytes);
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
                            uint8_t codec, uint16_t w, uint16_t h, uint8_t format)
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
            .frame_id = g_gpu.frame_id,
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

        esp_err_t err = espgpu_usb_send_chunk_iov(ESPGPU_CHUNK_FRAME, 0, &fh, hdr_sz, enc + off,
                                                  n, 50);
        if (err != ESP_OK) {
            return err;
        }
        off += n;
    }
    return ESP_OK;
}

/* ── 呈现任务 ─────────────────────────────────────────────────────── */

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

        /* 1. 生成内容（M2 起由光栅器替代） */
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
                         g_gpu.fb.height, g_gpu.fb.format);
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

/* ── 命令分发 ─────────────────────────────────────────────────────── */

static void handle_ctrl(const espgpu_ctrl_req_t *req, const uint8_t *extra, uint32_t extra_len)
{
    uint16_t status = ESPGPU_OK;
    ESP_LOGD(TAG, "ctrl op=0x%04X seq=%u arg0=%u len=%u", req->op, req->seq, req->arg0, extra_len);

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
        espgpu_usb_send_ctrl_resp(req->op, req->seq, ESPGPU_OK, &st, sizeof(st), 500);
        return;
    }

    case ESPGPU_CTRL_SET_MODE: {
        espgpu_mode_t m = g_gpu.mode;
        if (extra_len >= sizeof(m)) {
            memcpy(&m, extra, sizeof(m));
        }
        esp_err_t err = espgpu_gpu_set_mode(&m, (uint8_t)req->arg0);
        status = (err == ESP_OK) ? ESPGPU_OK : ESPGPU_ERR_NO_MEM;
        break;    }

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

    case ESPGPU_CTRL_REQUEST_KEYFRAME:
        /* 主机解码失败时用这个把两边重新对齐 */
        g_gpu.force_keyframe = true;
        break;

    default:
        status = ESPGPU_ERR_UNSUPPORTED;
        break;
    }

    ESP_LOGD(TAG, "ctrl op=0x%04X -> status=%u", req->op, status);
    espgpu_usb_send_ctrl_resp(req->op, req->seq, status, NULL, 0, 500);
}

/* ── 纹理管理 ─────────────────────────────────────────────────────── */

static void tex_free_pixels(espgpu_tex_t *t)
{
    if (t->tex.pixels) {
        heap_caps_free(t->tex.pixels);
        t->tex.pixels = NULL;
    }
    if (t->tex.alpha) {
        heap_caps_free(t->tex.alpha);
        t->tex.alpha = NULL;
    }
    t->tex.width = 0;
    t->tex.height = 0;
}

static espgpu_tex_t *tex_find(uint32_t id)
{
    for (int i = 0; i < ESPGPU_MAX_TEXTURE_SLOTS; i++) {
        if (g_gpu.textures[i].used && g_gpu.textures[i].id == id) {
            return &g_gpu.textures[i];
        }
    }
    return NULL;
}

static espgpu_tex_t *tex_get_slot(uint32_t id)
{
    espgpu_tex_t *t = tex_find(id);
    if (t) {
        return t;
    }
    for (int i = 0; i < ESPGPU_MAX_TEXTURE_SLOTS; i++) {
        if (!g_gpu.textures[i].used) {
            t = &g_gpu.textures[i];
            memset(t, 0, sizeof(*t));
            t->id = id;
            t->used = true;
            return t;
        }
    }
    /* 槽位耗尽：回收 0 号。M2 的 demo 不会超过 32 张纹理，M3 再上 LRU。 */
    t = &g_gpu.textures[0];
    tex_free_pixels(t);
    memset(t, 0, sizeof(*t));
    t->id = id;
    t->used = true;
    return t;
}

static esp_err_t cmd_upload_texture(const espgpu_upload_texture_t *u, const uint8_t *data,
                                    uint32_t len)
{
    if (u->width == 0 || u->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t pixels = (uint32_t)u->width * u->height;
    const uint32_t expected = pixels * 2; /* 源格式统一 16 位/像素：RGB565 或 RGBA4444 */
    if (u->total_len != expected) {
        ESP_LOGW(TAG, "texture %u: total_len %u != %u", u->id, u->total_len, expected);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((u->offset & 1u) || u->offset + len > expected) {
        return ESP_ERR_INVALID_SIZE;
    }

    espgpu_tex_t *t = tex_get_slot(u->id);

    if (u->offset == 0) {
        tex_free_pixels(t);
        t->tex.width = u->width;
        t->tex.height = u->height;
        t->format = u->format;
        t->total_len = expected;
        t->received = 0;

        t->tex.pixels = heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM);
        if (t->tex.pixels == NULL) {
            ESP_LOGE(TAG, "texture %u: pixel alloc failed (%u bytes)", u->id,
                     (unsigned)(pixels * 2));
            return ESP_ERR_NO_MEM;
        }
        if (u->format == ESPGPU_FMT_RGBA4444) {
            t->tex.alpha = heap_caps_malloc(pixels, MALLOC_CAP_SPIRAM);
            if (t->tex.alpha == NULL) {
                tex_free_pixels(t);
                return ESP_ERR_NO_MEM;
            }
        }
    } else if (t->total_len != expected || t->tex.width != u->width ||
               t->tex.height != u->height) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 上传时就展开成 RGB565 + alpha 平面，光栅器内层循环只管取 16 位 */
    const uint32_t first = u->offset / 2;
    const uint32_t n = len / 2;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t s;
        memcpy(&s, data + i * 2, 2);
        const uint32_t idx = first + i;
        if (u->format == ESPGPU_FMT_RGBA4444) {
            const uint32_t r = (s >> 12) & 0xF;
            const uint32_t g = (s >> 8) & 0xF;
            const uint32_t b = (s >> 4) & 0xF;
            const uint32_t a = s & 0xF;
            t->tex.pixels[idx] =
                (uint16_t)(((r * 31 / 15) << 11) | ((g * 63 / 15) << 5) | (b * 31 / 15));
            t->tex.alpha[idx] = (uint8_t)(a * 17);
        } else {
            t->tex.pixels[idx] = s;
        }
    }
    t->received += len;
    return ESP_OK;
}

/* ── PRESENT ──────────────────────────────────────────────────────── */

static esp_err_t cmd_present(void)
{
    const uint16_t w = g_gpu.surface.width;
    const uint16_t h = g_gpu.surface.height;
    const size_t bytes = (size_t)w * h * 2;

    if (g_gpu.enc_buf == NULL || g_gpu.shadow == NULL || bytes == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    /* enc_buf 是命令任务和呈现任务共用的。测试图案那一侧持有 g_gpu.lock，
     * 这里也必须持有，否则两边会交错写同一块缓冲，解出来是垃圾流。 */
    xSemaphoreTake(g_gpu.lock, portMAX_DELAY);

    uint8_t codec = g_gpu.frame_codec;
    if (codec == ESPGPU_CODEC_DELTA_RLE && g_gpu.force_keyframe) {
        codec = ESPGPU_CODEC_RLE;
    }
    /* 周期性关键帧：主机解码一旦失败，它的参考帧就和设备的 shadow 永久错位，
     * 表现为残影。每隔一段时间强制发一帧非差分，把两边重新对齐。 */
    if (codec == ESPGPU_CODEC_DELTA_RLE && g_gpu.frames_since_key >= 60) {
        codec = ESPGPU_CODEC_RLE;
    }

    espgpu_fb_encode_result_t res;
    /* 关键：只有 DELTA_RLE 才传参考帧。关键帧（RLE）如果也传了 shadow，
     * 编码端做了异或而解码端没有，解出来必然是垃圾（表现为残影和流长度越界）。 */
    const uint8_t *ref = (codec == ESPGPU_CODEC_DELTA_RLE) ? g_gpu.shadow : NULL;
    esp_err_t err = espgpu_encode_raw((const uint8_t *)g_gpu.surface.pixels, ref, bytes, 2,
                                      codec, g_gpu.enc_buf, g_gpu.enc_cap, &res);
    if (err != ESP_OK) {
        g_gpu.stats.frames_dropped++;
        return err;
    }

    err = send_frame(g_gpu.enc_buf, res.enc_bytes, res.raw_bytes, codec, w, h,
                     ESPGPU_FMT_RGB565);
    if (err == ESP_OK) {
        memcpy(g_gpu.shadow, g_gpu.surface.pixels, bytes);
        g_gpu.force_keyframe = false;
        if (codec == ESPGPU_CODEC_RLE) {
            g_gpu.frames_since_key = 0;
        } else {
            g_gpu.frames_since_key++;
        }
        g_gpu.frame_id++;
        g_gpu.stats.frames_presented++;
    } else {
        g_gpu.stats.frames_dropped++;
    }

    g_gpu.stats.frame_raw_bytes = res.raw_bytes;
    g_gpu.stats.frame_enc_bytes = res.enc_bytes;

    xSemaphoreGive(g_gpu.lock);
    return err;
}

/* ── 命令执行 ─────────────────────────────────────────────────────── */

static void exec_cmd(const espgpu_cmd_t *cmd, const uint8_t *body)
{
    switch (cmd->op) {
    case ESPGPU_OP_NOP:
        break;

    case ESPGPU_OP_CLEAR: {
        if (cmd->size < sizeof(espgpu_clear_t)) {
            break;
        }
        espgpu_clear_t c;
        memcpy(&c, body, sizeof(c));
        raster_clear(&g_gpu.surface, c.color);
        break;
    }

    case ESPGPU_OP_PIPELINE2D: {
        if (cmd->size < sizeof(espgpu_pipeline2d_t)) {
            break;
        }
        espgpu_pipeline2d_t p;
        memcpy(&p, body, sizeof(p));
        g_gpu.draw_state.blend = p.blend;
        g_gpu.draw_state.filter = p.filter;
        g_gpu.draw_state.wrap = p.wrap;
        g_gpu.draw_state.alpha_test = p.alpha_test;
        g_gpu.draw_state.alpha_ref = p.alpha_ref;
        break;
    }

    case ESPGPU_OP_BIND_TEXTURE: {
        if (cmd->size < sizeof(espgpu_bind_texture_t)) {
            break;
        }
        espgpu_bind_texture_t b;
        memcpy(&b, body, sizeof(b));
        if (b.unit < ESPGPU_MAX_TEXTURE_UNITS) {
            g_gpu.bound_texture[b.unit] = b.texture ? tex_find(b.texture) : NULL;
            if (g_gpu.bound_texture[b.unit]) {
                g_gpu.draw_state.filter = b.filter;
                g_gpu.draw_state.wrap = b.wrap;
            }
        }
        break;
    }

    case ESPGPU_OP_UPLOAD_TEXTURE: {
        if (cmd->size < sizeof(espgpu_upload_texture_t)) {
            break;
        }
        espgpu_upload_texture_t u;
        memcpy(&u, body, sizeof(u));
        esp_err_t err = cmd_upload_texture(&u, body + sizeof(u), cmd->size - sizeof(u));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "upload_texture %u failed: %s", u.id, esp_err_to_name(err));
        }
        break;
    }

    case ESPGPU_OP_DRAW: {
        if (cmd->size < sizeof(espgpu_draw_t)) {
            break;
        }
        espgpu_draw_t d;
        memcpy(&d, body, sizeof(d));
        const uint32_t avail = cmd->size - sizeof(d);
        const uint32_t stride = sizeof(espgpu_vertex2d_t);
        const uint32_t count = avail / stride;
        const uint8_t *verts = body + sizeof(d);
        espgpu_texture_t *tex =
            g_gpu.bound_texture[0] ? &g_gpu.bound_texture[0]->tex : NULL;

        if (d.prim == ESPGPU_PRIM_QUADS) {
            for (uint32_t i = 0; i + 3 < count; i += 4) {
                espgpu_vertex2d_t v[4];
                memcpy(v, verts + (size_t)i * stride, sizeof(v));
                raster_draw_quad(&g_gpu.surface, tex, v, &g_gpu.draw_state);
                g_gpu.stats.primitives++;
            }
            g_gpu.stats.draw_calls++;
            g_gpu.stats.vertices += count;
        }
        break;
    }

    case ESPGPU_OP_SCISSOR: {
        if (cmd->size < sizeof(espgpu_rect_t)) {
            break;
        }
        espgpu_rect_t r;
        memcpy(&r, body, sizeof(r));
        raster_set_scissor(&g_gpu.surface, (int16_t)r.x, (int16_t)r.y, (int16_t)r.w,
                           (int16_t)r.h);
        break;
    }

    case ESPGPU_OP_PRESENT:
        g_gpu.command_mode = true;
        cmd_present();
        break;

    default:
        g_gpu.stats.host_errors++;
        break;
    }
}

static void handle_cmd_chunk(const uint8_t *data, uint32_t len)
{
    /* 收到任何 GPU 命令就认为主机接管了渲染，停掉 M1 的测试图案自动出帧。
     * 切换瞬间必须强制关键帧：设备的 shadow 还是全零，而主机的参考帧里
     * 是测试图案的内容，不重置就必然错位。 */
    if (!g_gpu.command_mode) {
        g_gpu.force_keyframe = true;
    }
    g_gpu.command_mode = true;

    uint32_t off = 0;
    while (off + sizeof(espgpu_cmd_t) <= len) {
        espgpu_cmd_t cmd;
        memcpy(&cmd, data + off, sizeof(cmd));
        if (off + sizeof(cmd) + cmd.size > len) {
            g_gpu.stats.host_errors++;
            return;
        }
        g_gpu.stats.cmd_count++;
        exec_cmd(&cmd, data + off + sizeof(cmd));
        off += sizeof(cmd) + cmd.size;
    }
}

void espgpu_gpu_command_task(void *arg)
{
    (void)arg;

    espgpu_chunk_hdr_t hdr;

    while (1) {
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

        case ESPGPU_CHUNK_CMD:
            handle_cmd_chunk(s_cmd_buf, hdr.length);
            break;

        case ESPGPU_CHUNK_DATA:
            /* M2 起用于纹理 / 顶点缓冲上传 */
            g_gpu.stats.cmd_count++;
            break;

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

    g_gpu.lock = xSemaphoreCreateMutex();
    if (g_gpu.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_cmd_buf = heap_caps_malloc(ESPGPU_CHUNK_MAX_PAYLOAD, MALLOC_CAP_SPIRAM);
    if (s_cmd_buf == NULL) {
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

    /* M2：命令驱动渲染用的工作表面与差分参考帧 */
    if (!raster_surface_init(&g_gpu.surface, 320, 200)) {
        return ESP_ERR_NO_MEM;
    }
    g_gpu.shadow_bytes = (size_t)320 * 200 * 2;
    g_gpu.shadow = heap_caps_malloc(g_gpu.shadow_bytes, MALLOC_CAP_SPIRAM);
    if (g_gpu.shadow == NULL) {
        ESP_LOGE(TAG, "shadow buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(g_gpu.shadow, 0, g_gpu.shadow_bytes);

    g_gpu.draw_state.blend = ESPGPU_BLEND_NONE;
    g_gpu.draw_state.filter = ESPGPU_FILTER_NEAREST;
    g_gpu.draw_state.wrap = ESPGPU_WRAP_CLAMP;
    g_gpu.draw_state.alpha_test = 0;
    g_gpu.draw_state.alpha_ref = 0;
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
