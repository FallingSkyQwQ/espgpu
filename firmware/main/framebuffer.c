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
 * 最坏情况约为 size * (1 + 1/(128*bpp))（每 128 个字面像素一个控制字节；游程只会更小），
 * 输出缓冲按 size + size/bpp 分配，余量很大。
 */

#include <stdbool.h>
#include <stdint.h>
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
 *
 * 输出字节流与最初的「逐像素 LOAD()」实现逐字节一致（主机解码器不用改，
 * 等价性由差分测试覆盖）。相对原实现的改动都是为了 Xtensa（顺序执行、无分支预测）：
 *   - delta / upd 是编译期常量，三种组合各展开一份，热循环里没有 `ref ?` 判断
 *   - 字面量扫描用滑动窗口：每个像素只算一次 cur^ref（原来每像素 4~6 次），
 *     边扫描边写出，不再回头重新 LOAD 一遍
 *   - 游程扫描一次比较 32 位（u16 两像素 / u8 四像素）；差分帧里大片「没变」的
 *     零游程基本只剩访存
 *   - upd：差分时顺手把 cur 写回 ref（PSRAM 里的 shadow），省掉之后整帧
 *     memcpy 的一次 PSRAM 读 + 写；零游程（未变像素）不写，对应的 cache 行不会变脏
 */

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "rle word compare assumes little-endian"
#endif

typedef uint32_t __attribute__((may_alias)) rle_u32a_t;

#define RLE_MAX_RUN 129u
#define RLE_MAX_LIT 128u

static inline __attribute__((always_inline)) bool rle_aligned4(const void *p)
{
    return ((uintptr_t)p & 3u) == 0;
}

static inline __attribute__((always_inline)) size_t
rle16_impl(const uint16_t *cur, uint16_t *ref, uint8_t *out, size_t cap, size_t n,
           const bool delta, const bool upd)
{
#define L16(j) ((uint16_t)(delta ? (cur[j] ^ ref[j]) : cur[j]))
    const bool words = rle_aligned4(cur) && (!delta || rle_aligned4(ref));
    const rle_u32a_t *c32 = (const rle_u32a_t *)(const void *)cur;
    const rle_u32a_t *r32 = (const rle_u32a_t *)(const void *)ref;
    uint8_t tmp[1 + 2 * RLE_MAX_LIT];
    size_t o = 0;
    size_t i = 0;

    while (i < n) {
        const uint16_t v = L16(i);
        const size_t end = i + (n - i < RLE_MAX_RUN ? n - i : RLE_MAX_RUN);
        size_t j = i + 1;

        /* ── 游程：[i, j) 全等于 v ── */
        if (words) {
            if ((j & 1u) && j < end) {
                if (L16(j) != v) {
                    goto run_done;
                }
                j++;
            }
            const uint32_t vv = (uint32_t)v * 0x10001u;
            while (j + 2 <= end) {
                uint32_t w = c32[j >> 1];
                if (delta) {
                    w ^= r32[j >> 1];
                }
                if (w != vv) {
                    if ((uint16_t)w == v) {
                        j++; /* 小端：低半字是像素 j */
                    }
                    goto run_done;
                }
                j += 2;
            }
            if (j < end && L16(j) == v) {
                j++;
            }
        } else {
            while (j < end && L16(j) == v) {
                j++;
            }
        }
    run_done:
        if (j - i >= 2) {
            if (o + 3 > cap) {
                return 0;
            }
            out[o] = (uint8_t)(0x80u | (j - i - 2));
            memcpy(out + o + 1, &v, 2);
            o += 3;
            if (upd && v != 0) {
                for (size_t k = i; k < j; k++) {
                    ref[k] = cur[k];
                }
            }
            i = j;
            continue;
        }

        /* ── 字面量：从 i 开始，到「i 之后第一个三连等」/ 128 个 / 末尾为止 ── */
        const size_t lit = i;
        const size_t maxk = n - lit < RLE_MAX_LIT ? n - lit : RLE_MAX_LIT;
        const bool direct = cap - o >= 1 + 2 * maxk; /* 放得下最长字面量就直接写 */
        uint8_t *dst = direct ? out + o + 1 : tmp;
        size_t k = 0;
        uint16_t a = v;
#define PUT(px)                                        \
    do {                                               \
        memcpy(dst + 2 * k, &a, 2);                    \
        if (upd) {                                     \
            ref[px] = cur[px];                         \
        }                                              \
        k++;                                           \
    } while (0)
        PUT(lit);
        if (k < maxk) {
            /* 窗口：a = L(p), b = L(p+1)（p+1 < n 时有效），p = lit + k */
            a = L16(lit + 1);
            uint16_t b = lit + 2 < n ? L16(lit + 2) : 0;
            while (k < maxk) {
                const size_t p = lit + k;
                if (p + 2 < n) {
                    const uint16_t c = L16(p + 2);
                    if (a == b && b == c) {
                        break;
                    }
                    PUT(p);
                    a = b;
                    b = c;
                } else {
                    PUT(p);
                    a = b;
                }
            }
        }
#undef PUT
        if (direct) {
            out[o] = (uint8_t)(k - 1);
        } else {
            if (o + 1 + 2 * k > cap) {
                return 0;
            }
            out[o] = (uint8_t)(k - 1);
            memcpy(out + o + 1, tmp, 2 * k);
        }
        o += 1 + 2 * k;
        i = lit + k;
    }
    return o;
#undef L16
}

static inline __attribute__((always_inline)) size_t
rle8_impl(const uint8_t *cur, uint8_t *ref, uint8_t *out, size_t cap, size_t n,
          const bool delta, const bool upd)
{
#define L8(j) ((uint8_t)(delta ? (cur[j] ^ ref[j]) : cur[j]))
    const bool words = rle_aligned4(cur) && (!delta || rle_aligned4(ref));
    const rle_u32a_t *c32 = (const rle_u32a_t *)(const void *)cur;
    const rle_u32a_t *r32 = (const rle_u32a_t *)(const void *)ref;
    uint8_t tmp[1 + RLE_MAX_LIT];
    size_t o = 0;
    size_t i = 0;

    while (i < n) {
        const uint8_t v = L8(i);
        const size_t end = i + (n - i < RLE_MAX_RUN ? n - i : RLE_MAX_RUN);
        size_t j = i + 1;

        if (words) {
            while ((j & 3u) && j < end) {
                if (L8(j) != v) {
                    goto run_done;
                }
                j++;
            }
            const uint32_t vv = (uint32_t)v * 0x01010101u;
            while (j + 4 <= end) {
                uint32_t w = c32[j >> 2];
                if (delta) {
                    w ^= r32[j >> 2];
                }
                w ^= vv;
                if (w != 0) {
                    /* 小端：最低的非零字节就是第一个不等的像素 */
                    j += (unsigned)__builtin_ctz(w) >> 3;
                    goto run_done;
                }
                j += 4;
            }
            while (j < end && L8(j) == v) {
                j++;
            }
        } else {
            while (j < end && L8(j) == v) {
                j++;
            }
        }
    run_done:
        if (j - i >= 2) {
            if (o + 2 > cap) {
                return 0;
            }
            out[o] = (uint8_t)(0x80u | (j - i - 2));
            out[o + 1] = v;
            o += 2;
            if (upd && v != 0) {
                memcpy(ref + i, cur + i, j - i);
            }
            i = j;
            continue;
        }

        const size_t lit = i;
        const size_t maxk = n - lit < RLE_MAX_LIT ? n - lit : RLE_MAX_LIT;
        const bool direct = cap - o >= 1 + maxk;
        uint8_t *dst = direct ? out + o + 1 : tmp;
        size_t k = 0;
        uint8_t a = v;
        dst[k++] = a;
        if (k < maxk) {
            a = L8(lit + 1);
            uint8_t b = lit + 2 < n ? L8(lit + 2) : 0;
            while (k < maxk) {
                const size_t p = lit + k;
                if (p + 2 < n) {
                    const uint8_t c = L8(p + 2);
                    if (a == b && b == c) {
                        break;
                    }
                    dst[k++] = a;
                    a = b;
                    b = c;
                } else {
                    dst[k++] = a;
                    a = b;
                }
            }
        }
        if (upd) {
            memcpy(ref + lit, cur + lit, k);
        }
        if (direct) {
            out[o] = (uint8_t)(k - 1);
        } else {
            if (o + 1 + k > cap) {
                return 0;
            }
            out[o] = (uint8_t)(k - 1);
            memcpy(out + o + 1, tmp, k);
        }
        o += 1 + k;
        i = lit + k;
    }
    return o;
#undef L8
}

/* 三种组合各一份：关键帧 / 只读参考帧 / 参考帧原地更新 */
IRAM_ATTR static size_t rle16_key(const uint8_t *cur, uint8_t *out, size_t cap, size_t n)
{
    return rle16_impl((const uint16_t *)(const void *)cur, NULL, out, cap, n, false, false);
}
IRAM_ATTR static size_t rle16_delta(const uint8_t *cur, const uint8_t *ref, uint8_t *out,
                                    size_t cap, size_t n)
{
    return rle16_impl((const uint16_t *)(const void *)cur, (uint16_t *)(uintptr_t)ref, out, cap,
                      n, true, false);
}
IRAM_ATTR static size_t rle16_delta_upd(const uint8_t *cur, uint8_t *ref, uint8_t *out,
                                        size_t cap, size_t n)
{
    return rle16_impl((const uint16_t *)(const void *)cur, (uint16_t *)(void *)ref, out, cap, n,
                      true, true);
}
/* u8 只在测试图案的 INDEX8 模式下用，不是热路径：放 flash，省 IRAM */
static size_t rle8_key(const uint8_t *cur, uint8_t *out, size_t cap, size_t n)
{
    return rle8_impl(cur, NULL, out, cap, n, false, false);
}
static size_t rle8_delta(const uint8_t *cur, const uint8_t *ref, uint8_t *out, size_t cap,
                         size_t n)
{
    return rle8_impl(cur, (uint8_t *)(uintptr_t)ref, out, cap, n, true, false);
}
static size_t rle8_delta_upd(const uint8_t *cur, uint8_t *ref, uint8_t *out, size_t cap,
                             size_t n)
{
    return rle8_impl(cur, ref, out, cap, n, true, true);
}

static esp_err_t encode_common(const uint8_t *cur, uint8_t *ref, bool upd, size_t bytes,
                               unsigned bpp, uint8_t codec, uint8_t *out, size_t cap,
                               espgpu_fb_encode_result_t *result)
{
    if (cur == NULL || out == NULL || (bpp != 1 && bpp != 2)) {
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
    case ESPGPU_CODEC_DELTA_RLE: {
        /* 与原实现一致：RLE 也接受 ref（调用者负责只在差分时传） */
        const size_t n = bytes / bpp;
        if (bpp == 1) {
            encoded = ref == NULL ? rle8_key(cur, out, cap, n)
                      : upd       ? rle8_delta_upd(cur, ref, out, cap, n)
                                  : rle8_delta(cur, ref, out, cap, n);
        } else {
            encoded = ref == NULL ? rle16_key(cur, out, cap, n)
                      : upd       ? rle16_delta_upd(cur, ref, out, cap, n)
                                  : rle16_delta(cur, ref, out, cap, n);
        }
        if (encoded == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        break;
    }

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (result) {
        result->raw_bytes = (uint32_t)bytes;
        result->enc_bytes = (uint32_t)encoded;
    }
    return ESP_OK;
}

esp_err_t espgpu_encode_raw(const uint8_t *cur, const uint8_t *ref, size_t bytes, unsigned bpp,
                            uint8_t codec, uint8_t *out, size_t cap,
                            espgpu_fb_encode_result_t *result)
{
    return encode_common(cur, (uint8_t *)(uintptr_t)ref, false, bytes, bpp, codec, out, cap,
                         result);
}

esp_err_t espgpu_encode_update(const uint8_t *cur, uint8_t *shadow, size_t bytes, unsigned bpp,
                               uint8_t codec, uint8_t *out, size_t cap,
                               espgpu_fb_encode_result_t *result)
{
    if (shadow == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (codec != ESPGPU_CODEC_DELTA_RLE) {
        const esp_err_t err = encode_common(cur, NULL, false, bytes, bpp, codec, out, cap, result);
        if (err == ESP_OK) {
            memcpy(shadow, cur, bytes);
        }
        return err;
    }
    return encode_common(cur, shadow, true, bytes, bpp, codec, out, cap, result);
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
