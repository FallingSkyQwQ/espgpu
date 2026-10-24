/*
 * framebuffer.c — 双缓冲 + 测试图案 + 帧编码
 *
 * 编码格式（RLE 部分，host 端解码需与此严格一致）：
 *
 *   控制字节 c
 *     c & 0x80 : 重复游程，像素数 = (c & 0x7F) + 2，范围 [2, 129]，
 *                后接 bpp 字节的像素值
 *     c & 0x80 == 0 : 字面游程，像素数 = c + 1，范围 [1, 128]，
 *                后接 count * bpp 字节的原始像素
 *
 * 最坏情况（全字面）膨胀为 bpp+1 字节/像素，所以输出缓冲按 size + size/bpp 分配。
 */

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "espgpu_proto.h"
#include "framebuffer.h"

static const char *TAG = "fb";

/* ── 生命周期 ─────────────────────────────────────────────────────── */

esp_err_t espgpu_fb_init(espgpu_fb_t *fb, uint16_t w, uint16_t h, uint8_t format)
{
    if (fb == NULL || w == 0 || h == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t bpp;
    switch (format) {
    case ESPGPU_FMT_INDEX8:
    case ESPGPU_FMT_R8:
        bpp = 1;
        break;
    case ESPGPU_FMT_RGB565:
    case ESPGPU_FMT_RGBA4444:
    case ESPGPU_FMT_DEPTH16:
        bpp = 2;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    memset(fb, 0, sizeof(*fb));
    fb->width = w;
    fb->height = h;
    fb->format = format;
    fb->bpp = bpp;
    fb->size = (size_t)w * h * bpp;

    for (int i = 0; i < 2; i++) {
        fb->buf[i] = heap_caps_malloc(fb->size, MALLOC_CAP_SPIRAM);
        if (fb->buf[i] == NULL) {
            ESP_LOGE(TAG, "PSRAM alloc failed for buffer %d (%u bytes)", i,
                     (unsigned)fb->size);
            espgpu_fb_deinit(fb);
            return ESP_ERR_NO_MEM;
        }
        memset(fb->buf[i], 0, fb->size);
    }

    ESP_LOGI(TAG, "framebuffer %ux%u fmt=%u bpp=%u, %u bytes x2 (PSRAM)", w, h, format, bpp,
             (unsigned)fb->size);
    return ESP_OK;
}

void espgpu_fb_deinit(espgpu_fb_t *fb)
{
    for (int i = 0; i < 2; i++) {
        if (fb->buf[i]) {
            heap_caps_free(fb->buf[i]);
            fb->buf[i] = NULL;
        }
    }
}

/* ── 编码 ─────────────────────────────────────────────────────────── */

size_t espgpu_fb_max_encoded(const espgpu_fb_t *fb)
{
    return fb->size + fb->size / fb->bpp;
}

/*
 * RLE 核心，u8 / u16 两个特化版本，避免热循环里的 bpp 分支。
 * ref 非空时对参考帧做异或（差分模式）。
 */

IRAM_ATTR static size_t rle_u8(const uint8_t *cur, const uint8_t *ref, uint8_t *out, size_t cap,
                     size_t count)
{
#define LOAD(i) (ref ? (uint8_t)(cur[i] ^ ref[i]) : cur[i])
    size_t o = 0;
    size_t i = 0;
    while (i < count) {
        uint8_t v = LOAD(i);
        size_t run = 1;
        while (i + run < count && run < 129 && LOAD(i + run) == v) {
            run++;
        }
        if (run >= 2) {
            if (o + 2 > cap) {
                return 0;
            }
            out[o++] = (uint8_t)(0x80 | (run - 2));
            out[o++] = v;
            i += run;
            continue;
        }
        size_t lit = i;
        i++;
        while (i < count && (i - lit) < 128) {
            if (i + 2 < count && LOAD(i) == LOAD(i + 1) && LOAD(i + 1) == LOAD(i + 2)) {
                break;
            }
            i++;
        }
        size_t n = i - lit;
        if (o + 1 + n > cap) {
            return 0;
        }
        out[o++] = (uint8_t)(n - 1);
        for (size_t k = 0; k < n; k++) {
            out[o++] = LOAD(lit + k);
        }
    }
    return o;
#undef LOAD
}

IRAM_ATTR static size_t rle_u16(const uint8_t *curb, const uint8_t *refb, uint8_t *out, size_t cap,
                      size_t total_pixels)
{
    const uint16_t *cur = (const uint16_t *)curb;
    const uint16_t *ref = (const uint16_t *)refb;
    size_t o = 0;
    size_t i = 0;
#define LOAD(i) (uint16_t)(ref ? (cur[i] ^ ref[i]) : cur[i])
    const size_t count = total_pixels;
    while (i < count) {
        uint16_t v = LOAD(i);
        size_t run = 1;
        while (i + run < count && run < 129 && LOAD(i + run) == v) {
            run++;
        }
        if (run >= 2) {
            if (o + 3 > cap) {
                return 0;
            }
            out[o++] = (uint8_t)(0x80 | (run - 2));
            memcpy(out + o, &v, 2);
            o += 2;
            i += run;
            continue;
        }
        size_t lit = i;
        i++;
        while (i < count && (i - lit) < 128) {
            if (i + 2 < count && LOAD(i) == LOAD(i + 1) && LOAD(i + 1) == LOAD(i + 2)) {
                break;
            }
            i++;
        }
        size_t n = i - lit;
        if (o + 1 + n * 2 > cap) {
            return 0;
        }
        out[o++] = (uint8_t)(n - 1);
        for (size_t k = 0; k < n; k++) {
            uint16_t t = LOAD(lit + k);
            memcpy(out + o, &t, 2);
            o += 2;
        }
    }
    return o;
#undef LOAD
}

IRAM_ATTR esp_err_t espgpu_encode_raw(const uint8_t *cur, const uint8_t *ref, size_t bytes, unsigned bpp,
                            uint8_t codec, uint8_t *out, size_t cap,
                            espgpu_fb_encode_result_t *result)
{
    if (cur == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t encoded = 0;
    switch (codec) {
    case ESPGPU_CODEC_RAW:
        if (cap < bytes) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, cur, bytes);
        encoded = bytes;
        break;

    case ESPGPU_CODEC_RLE:
    case ESPGPU_CODEC_DELTA_RLE:
        if (bpp == 1) {
            encoded = rle_u8(cur, ref, out, cap, bytes);
        } else {
            encoded = rle_u16(cur, ref, out, cap, bytes / 2);
        }
        if (encoded == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        break;

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (result) {
        result->raw_bytes = (uint32_t)bytes;
        result->enc_bytes = (uint32_t)encoded;
    }
    return ESP_OK;
}

esp_err_t espgpu_fb_encode(espgpu_fb_t *fb, uint8_t codec, uint8_t *out, size_t out_cap,
                           espgpu_fb_encode_result_t *result)
{
    if (fb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *cur = espgpu_fb_draw(fb);
    const uint8_t *ref = (codec == ESPGPU_CODEC_DELTA_RLE) ? espgpu_fb_ref(fb) : NULL;

    return espgpu_encode_raw(cur, ref, fb->size, fb->bpp, codec, out, out_cap, result);
}

/* ── 测试图案 ─────────────────────────────────────────────────────── */

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3));
}

static inline void put_px(espgpu_fb_t *fb, uint8_t *row, uint32_t x, uint32_t c)
{
    if (fb->bpp == 1) {
        /* INDEX8 模式：把 RGB565 折成亮度索引。
         * 默认 CLUT 是灰阶，所以同一套测试图案在两种格式下视觉一致，
         * 便于横向比较带宽。真实场景的 CLUT 由 host 通过 SET_CLUT 下发。 */
        uint16_t v = (uint16_t)c;
        uint32_t r = (v >> 11) & 0x1F;
        uint32_t g = (v >> 5) & 0x3F;
        uint32_t b = v & 0x1F;
        row[x] = (uint8_t)((r * 7 + g * 13 + b * 3) >> 3);
    } else {
        uint16_t v = (uint16_t)c;
        memcpy(row + x * 2, &v, 2);
    }
}

void espgpu_fb_default_clut(uint16_t out[256])
{
    /* 默认灰阶调色板。INDEX8 下所有测试图案显示为灰度，
     * 与 RGB565 一一对应，方便确认链路正确性。
     * 接入真实调色板（如 Doom 的 PLAYPAL）时由 host 下发 SET_CLUT 覆盖。 */
    for (int i = 0; i < 256; i++) {
        out[i] = rgb565((uint8_t)i, (uint8_t)i, (uint8_t)i);
    }
}

/*
 * 图案生成里的坐标数学必须全部用有符号运算并显式夹取：
 * 分辨率是可变的，一旦某个减法的被除数/模数变成 0 或下溢，
 * 轻则除零 panic，重则越界写破坏内存（曾经在 160x120 上触发过）。
 */
static inline uint32_t wrap_u32(uint32_t v, uint32_t span)
{
    return span ? (v % span) : 0;
}

void espgpu_fb_test_pattern(espgpu_fb_t *fb, uint32_t frame, uint8_t pattern)
{
    uint8_t *base = espgpu_fb_draw(fb);
    const uint32_t w = fb->width;
    const uint32_t h = fb->height;

    /* 简单确定性伪随机，帧间连续但不重复 */
    uint32_t rng = 0x12345678u ^ (frame * 2654435761u);

    switch (pattern) {
    case ESPGPU_PATTERN_BARS: {
        const uint32_t bar_w = 16;
        const uint32_t shift = wrap_u32(frame * 3, w + bar_w);
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = base + (size_t)y * w * fb->bpp;
            for (uint32_t x = 0; x < w; x++) {
                uint32_t bx = (x + shift) / bar_w;
                uint32_t shade = (y < h / 2) ? 255u : 160u;
                uint8_t r = (uint8_t)((bx * 37u) & 0xFF);
                uint8_t g = (uint8_t)((((bx * 73u) & 0xFF) * shade) / 255);
                uint8_t b = (uint8_t)((((bx * 149u) & 0xFF) * shade) / 255);
                put_px(fb, row, x, rgb565(r, g, b));
            }
        }
        break;
    }

    case ESPGPU_PATTERN_UI: {
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = base + (size_t)y * w * fb->bpp;
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c;
                if (x % 32 == 0 || y % 32 == 0) {
                    c = rgb565(0x30, 0x38, 0x48); /* 网格 */
                } else if (y < h / 8) {
                    c = rgb565(0x20, 0x28, 0x38); /* 标题栏 */
                } else if (x > w / 3 && x < w / 3 * 2 && y > h / 4 && y < h / 2) {
                    c = rgb565(0x54, 0x5c, 0x70); /* 面板 */
                } else {
                    c = rgb565(0x14, 0x18, 0x20); /* 背景 */
                }
                put_px(fb, row, x, c);
            }
        }

        /* 移动方块，带边框与内部噪声 —— 拉低压缩率，模拟精灵 */
        const uint32_t sprite = 48;
        const uint32_t max_bx = (w > sprite) ? (w - sprite) : 0;
        const uint32_t max_by = (h > sprite) ? (h - sprite) : 0;
        const uint32_t bx = wrap_u32(frame * 7, max_bx + 1);
        const uint32_t by = wrap_u32(frame * 5, max_by + 1);

        for (uint32_t y = by; y < by + sprite && y < h; y++) {
            uint8_t *row = base + (size_t)y * w * fb->bpp;
            for (uint32_t x = bx; x < bx + sprite && x < w; x++) {
                const bool edge = (x == bx || x == bx + sprite - 1 || y == by ||
                                   y == by + sprite - 1);
                rng = rng * 1664525u + 1013904223u;
                uint16_t c = edge ? rgb565(0xF0, 0xC0, 0x40)
                                  : (uint16_t)(0x8000u | ((rng >> 19) & 0x1F) |
                                               (((rng >> 13) & 0x3F) << 5));
                put_px(fb, row, x, c);
            }
        }
        break;
    }

    case ESPGPU_PATTERN_DOOMISH: {
        /* 近似 Doom 的视野结构：上方天花板、下方地板、中间竖条墙面。
         * 这组图案的压缩率最接近真实目标场景。 */
        const uint32_t horizon = h * 45 / 100;
        const uint32_t floor_y = h * 62 / 100;
        const uint32_t floor_span = (h > floor_y) ? (h - floor_y) : 1;
        const uint32_t shift = wrap_u32(frame * 2, w ? w : 1);

        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = base + (size_t)y * w * fb->bpp;
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c;
                if (y < horizon) {
                    uint8_t v = (uint8_t)(20 + (horizon ? y * 60 / horizon : 0));
                    c = rgb565((uint8_t)(v / 2), (uint8_t)(v / 2), v);
                } else if (y > floor_y) {
                    int32_t d = (int32_t)y - (int32_t)floor_y;
                    int32_t v = 90 - d * 50 / (int32_t)floor_span;
                    if (v < 0) {
                        v = 0;
                    }
                    c = rgb565((uint8_t)v, (uint8_t)(v * 3 / 4), (uint8_t)(v / 3));
                } else {
                    uint32_t band = ((x + shift) / 24) & 3;
                    uint8_t v = (uint8_t)(60 + band * 40);
                    if (wrap_u32(x + shift, 24) < 2) {
                        v = 24; /* 墙面分隔线 */
                    }
                    c = rgb565(v, v, (uint8_t)(v * 4 / 5));
                }
                put_px(fb, row, x, c);
            }
        }

        /* 一个移动的敌人精灵 */
        const uint32_t sprite_w = 32;
        const uint32_t sprite_h = 40;
        const uint32_t max_sx = (w > sprite_w) ? (w - sprite_w) : 0;
        const uint32_t sx = wrap_u32(frame * 4, max_sx + 1);
        const uint32_t sy = (horizon > sprite_h / 2) ? (horizon - sprite_h / 2) : 0;

        for (uint32_t y = sy; y < sy + sprite_h && y < h; y++) {
            uint8_t *row = base + (size_t)y * w * fb->bpp;
            for (uint32_t x = sx; x < sx + sprite_w && x < w; x++) {
                rng = rng * 1664525u + 1013904223u;
                if ((rng & 3) == 0) {
                    continue; /* 留出透明感 */
                }
                uint16_t c = rgb565(0xC0, (uint8_t)(0x30 + ((rng >> 8) & 0x3F)), 0x30);
                put_px(fb, row, x, c);
            }
        }
        break;
    }

    case ESPGPU_PATTERN_NOISE: {
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = base + (size_t)y * w * fb->bpp;
            for (uint32_t x = 0; x < w; x++) {
                rng = rng * 1664525u + 1013904223u;
                put_px(fb, row, x, (rng >> 8) & 0xFFFF);
            }
        }
        break;
    }

    case ESPGPU_PATTERN_MIX:
    default: {
        const uint32_t half = h / 2;
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = base + (size_t)y * w * fb->bpp;
            for (uint32_t x = 0; x < w; x++) {
                if (y < half) {
                    uint16_t c = (x / 32 + y / 32) & 1 ? rgb565(0x28, 0x30, 0x40)
                                                       : rgb565(0x18, 0x20, 0x2C);
                    put_px(fb, row, x, c);
                } else {
                    rng = rng * 1664525u + 1013904223u;
                    put_px(fb, row, x, (rng >> 8) & 0xFFFF);
                }
            }
        }
        break;
    }
    }
}
