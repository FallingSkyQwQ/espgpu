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
    snprintf(di->chip_model, sizeof(di->chip_model), "ESP32-S3 r%d", ci.revision / 100);

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

esp_err_t espgpu_gpu_set_mode(const espgpu_mode_t *mode, uint8_t pattern)
{
    if (mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(g_gpu.lock, portMAX_DELAY);

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

    g_gpu.mode = m;
    g_gpu.pattern = pattern < ESPGPU_PATTERN_COUNT ? pattern : ESPGPU_PATTERN_DOOMISH;
    g_gpu.present_period_us = 1000000u / m.max_fps;

    /* 分辨率/格式/图案一变，主机端的差分参考帧就失效了。
     * 强制下一帧走非差分编码，让主机重建参考帧，否则会解出花屏。 */
    g_gpu.force_keyframe = true;

    xSemaphoreGive(g_gpu.lock);

    ESP_LOGI(TAG, "mode %ux%u fmt=%u codec=%u fps=%u pattern=%u", m.width, m.height,
             m.format, m.codec, m.max_fps, g_gpu.pattern);
    return err;
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
                            uint8_t codec)
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
            .width = g_gpu.fb.width,
            .height = g_gpu.fb.height,
            .origin_x = 0,
            .origin_y = 0,
            .origin_w = g_gpu.fb.width,
            .origin_h = g_gpu.fb.height,
            .data_len = n,
            .raw_len = raw_len,
            .format = g_gpu.fb.format,
            .codec = codec,
            .frag_index = (uint8_t)i,
            .frag_count = (uint8_t)frag_count,
            .flags = flags,
            .palette_dirty = 0,
        };

        esp_err_t err = espgpu_usb_send_chunk_iov(ESPGPU_CHUNK_FRAME, 0, &fh, hdr_sz, enc + off,
                                                  n, 250);
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

        if (!g_gpu.streaming || !espgpu_usb_ready() || g_gpu.enc_buf == NULL) {
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
        err = send_frame(g_gpu.enc_buf, res.enc_bytes, res.raw_bytes, codec);
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

        /* 4. 帧率控制：按剩余预算休眠 */
        int64_t elapsed = esp_timer_get_time() - t0;
        int64_t remain = (int64_t)g_gpu.present_period_us - elapsed;
        if (remain > 2000) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(remain / 1000)));
        } else if (remain > 0) {
            taskYIELD();
        }
    }
}

/* ── 命令分发 ─────────────────────────────────────────────────────── */

static void handle_ctrl(const espgpu_ctrl_req_t *req, const uint8_t *extra, uint32_t extra_len)
{
    uint16_t status = ESPGPU_OK;

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

    default:
        status = ESPGPU_ERR_UNSUPPORTED;
        break;
    }

    espgpu_usb_send_ctrl_resp(req->op, req->seq, status, NULL, 0, 500);
}

static void handle_cmd_chunk(const uint8_t *data, uint32_t len)
{
    /* M1 还没有光栅器，命令流只做计数与识别，真正的执行在 M2 接入。 */
    uint32_t off = 0;
    while (off + sizeof(espgpu_cmd_t) <= len) {
        espgpu_cmd_t cmd;
        memcpy(&cmd, data + off, sizeof(cmd));
        if (off + sizeof(cmd) + cmd.size > len) {
            g_gpu.stats.host_errors++;
            return;
        }

        switch (cmd.op) {
        case ESPGPU_OP_PRESENT:
            /* 由呈现任务自行驱动，这里显式请求一次不额外处理 */
            break;
        case ESPGPU_OP_NOP:
            break;
        default:
            g_gpu.stats.cmd_count++;
            break;
        }
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
