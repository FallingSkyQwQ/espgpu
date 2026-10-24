/*
 * raster.c — 2D 光栅器
 *
 * 设计要点：
 *  - 工作表面分配在内部 SRAM（失败才退回 PSRAM）。光栅器直写 PSRAM 会和
 *    USB DMA 抢总线，是 S3 上最容易踩的性能陷阱。
 *  - 四边形拆两个三角形，用边缘函数做定点内测试，UV 用浮点累加
 *    （S3 有单精度 FPU，每像素两次加法的开销可以接受）。
 *  - 顶点色按平面处理（取 v[0]），M2 的 2D 内容不需要高洛德。
 */

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_memory_utils.h"

#include "raster.h"

static const char *TAG = "raster";

/* ── 生命周期 ─────────────────────────────────────────────────────── */

bool raster_surface_init(espgpu_surface_t *s, uint16_t w, uint16_t h)
{
    memset(s, 0, sizeof(*s));
    const size_t bytes = (size_t)w * h * 2;

    s->pixels = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s->pixels == NULL) {
        ESP_LOGW(TAG, "internal SRAM alloc failed (%u bytes), falling back to PSRAM",
                 (unsigned)bytes);
        s->pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    if (s->pixels == NULL) {
        ESP_LOGE(TAG, "surface alloc failed (%u bytes)", (unsigned)bytes);
        return false;
    }

    s->width = w;
    s->height = h;
    raster_set_scissor(s, 0, 0, (int16_t)w, (int16_t)h);
    memset(s->pixels, 0, bytes);
    ESP_LOGI(TAG, "surface %ux%u, %u bytes (%s)", w, h, (unsigned)bytes,
             esp_ptr_internal(s->pixels) ? "internal SRAM" : "PSRAM");
    return true;
}

void raster_surface_deinit(espgpu_surface_t *s)
{
    if (s->pixels) {
        heap_caps_free(s->pixels);
        s->pixels = NULL;
    }
}

void raster_set_scissor(espgpu_surface_t *s, int16_t x, int16_t y, int16_t w, int16_t h)
{
    /* 夹到表面范围内，允许完全为空的裁剪区 */
    if (x < 0) {
        w = (int16_t)(w + x);
        x = 0;
    }
    if (y < 0) {
        h = (int16_t)(h + y);
        y = 0;
    }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (x + w > (int16_t)s->width) w = (int16_t)(s->width - x);
    if (y + h > (int16_t)s->height) h = (int16_t)(s->height - y);
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    s->scissor_x = x;
    s->scissor_y = y;
    s->scissor_w = w;
    s->scissor_h = h;
}

IRAM_ATTR void raster_clear(espgpu_surface_t *s, uint16_t color)
{
    for (int16_t y = 0; y < s->scissor_h; y++) {
        uint16_t *row = s->pixels + (size_t)(s->scissor_y + y) * s->width + s->scissor_x;
        for (int16_t x = 0; x < s->scissor_w; x++) {
            row[x] = color;
        }
    }
}

/* ── 颜色 ─────────────────────────────────────────────────────────── */

static inline void unpack565(uint16_t c, uint32_t *r, uint32_t *g, uint32_t *b)
{
    *r = (c >> 11) & 0x1F;
    *g = (c >> 5) & 0x3F;
    *b = c & 0x1F;
}

static inline uint16_t pack565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F));
}

IRAM_ATTR uint16_t raster_blend_rgb565(uint16_t src, uint16_t dst, uint8_t alpha, uint8_t mode)
{
    if (mode == ESPGPU_BLEND_NONE || alpha == 255) {
        return src;
    }
    if (alpha == 0) {
        return dst;
    }

    uint32_t sr, sg, sb, dr, dg, db;
    unpack565(src, &sr, &sg, &sb);
    unpack565(dst, &dr, &dg, &db);

    uint32_t r, g, b;
    switch (mode) {
    case ESPGPU_BLEND_ADD:
        r = sr + dr; if (r > 31) r = 31;
        g = sg + dg; if (g > 63) g = 63;
        b = sb + db; if (b > 31) b = 31;
        break;
    case ESPGPU_BLEND_MUL:
        r = sr * dr / 31;
        g = sg * dg / 63;
        b = sb * db / 31;
        break;
    case ESPGPU_BLEND_ALPHA:
    default:
        r = (sr * alpha + dr * (255 - alpha)) / 255;
        g = (sg * alpha + dg * (255 - alpha)) / 255;
        b = (sb * alpha + db * (255 - alpha)) / 255;
        break;
    }
    return pack565(r, g, b);
}

/* ── 纹理采样 ─────────────────────────────────────────────────────── */

static inline int32_t wrap_coord(int32_t v, int32_t n, uint8_t wrap)
{
    if (n <= 0) {
        return 0;
    }
    if (wrap == ESPGPU_WRAP_CLAMP) {
        if (v < 0) return 0;
        if (v >= n) return n - 1;
        return v;
    }
    /* 快速路径：内层循环会把 REPEAT 的坐标维持在 [0,n)，
     * 这里直接返回，避免每像素两次软件除法。 */
    if (v >= 0 && v < n) {
        return v;
    }
    if (wrap == ESPGPU_WRAP_REPEAT) {
        v %= n;
        if (v < 0) {
            v += n;
        }
        return v;
    }
    /* MIRROR */
    v %= (n * 2);
    if (v < 0) {
        v += n * 2;
    }
    return (v < n) ? v : (n * 2 - 1 - v);
}

static inline uint8_t tex_alpha(const espgpu_texture_t *t, int32_t x, int32_t y)
{
    if (t->alpha == NULL) {
        return 255;
    }
    return t->alpha[(size_t)y * t->width + x];
}

/* 不加 IRAM_ATTR：加了之后编译器往往拒绝内联，而它是每像素都要调的 */
static inline uint16_t tex_sample(const espgpu_texture_t *t, float u, float v, uint8_t filter,
                                  uint8_t wrap, uint8_t *alpha_out)
{
    if (filter == ESPGPU_FILTER_LINEAR) {
        const float fu = u - 0.5f;
        const float fv = v - 0.5f;
        const int32_t x0f = (int32_t)(fu >= 0 ? fu : fu - 1.0f);
        const int32_t y0f = (int32_t)(fv >= 0 ? fv : fv - 1.0f);
        const float tx = fu - (float)x0f;
        const float ty = fv - (float)y0f;

        const int32_t x0 = wrap_coord(x0f, t->width, wrap);
        const int32_t y0 = wrap_coord(y0f, t->height, wrap);
        const int32_t x1 = wrap_coord(x0f + 1, t->width, wrap);
        const int32_t y1 = wrap_coord(y0f + 1, t->height, wrap);

        const uint16_t c00 = t->pixels[(size_t)y0 * t->width + x0];
        const uint16_t c10 = t->pixels[(size_t)y0 * t->width + x1];
        const uint16_t c01 = t->pixels[(size_t)y1 * t->width + x0];
        const uint16_t c11 = t->pixels[(size_t)y1 * t->width + x1];

        uint32_t r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
        unpack565(c00, &r0, &g0, &b0);
        unpack565(c10, &r1, &g1, &b1);
        unpack565(c01, &r2, &g2, &b2);
        unpack565(c11, &r3, &g3, &b3);

        const uint32_t w00 = (uint32_t)((1.0f - tx) * (1.0f - ty) * 256.0f);
        const uint32_t w10 = (uint32_t)(tx * (1.0f - ty) * 256.0f);
        const uint32_t w01 = (uint32_t)((1.0f - tx) * ty * 256.0f);
        const uint32_t w11 = (uint32_t)(tx * ty * 256.0f);

        const uint32_t r = (r0 * w00 + r1 * w10 + r2 * w01 + r3 * w11) >> 8;
        const uint32_t g = (g0 * w00 + g1 * w10 + g2 * w01 + g3 * w11) >> 8;
        const uint32_t b = (b0 * w00 + b1 * w10 + b2 * w01 + b3 * w11) >> 8;

        if (alpha_out) {
            *alpha_out = (uint8_t)(((uint32_t)tex_alpha(t, x0, y0) * w00 +
                                    (uint32_t)tex_alpha(t, x1, y0) * w10 +
                                    (uint32_t)tex_alpha(t, x0, y1) * w01 +
                                    (uint32_t)tex_alpha(t, x1, y1) * w11) >> 8);
        }
        return pack565(r, g, b);
    }

    /* 最近邻 */
    const int32_t ix = wrap_coord((int32_t)u, t->width, wrap);
    const int32_t iy = wrap_coord((int32_t)v, t->height, wrap);
    if (alpha_out) {
        *alpha_out = tex_alpha(t, ix, iy);
    }
    return t->pixels[(size_t)iy * t->width + ix];
}

/* ── 三角形光栅化 ─────────────────────────────────────────────────── */

IRAM_ATTR static void draw_tri(espgpu_surface_t *s, const espgpu_texture_t *tex,
                     espgpu_vertex2d_t a, espgpu_vertex2d_t b, espgpu_vertex2d_t c,
                     const espgpu_draw_state_t *st, uint16_t flat_color)
{
    /* 1/16 像素定点 */
    int32_t area = (int32_t)(b.x - a.x) * (int32_t)(c.y - a.y) -
                   (int32_t)(b.y - a.y) * (int32_t)(c.x - a.x);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        espgpu_vertex2d_t t = b;
        b = c;
        c = t;
        area = -area;
    }

    /* 包围盒（像素单位），夹到 scissor */
    int32_t minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    int32_t maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    int32_t miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    int32_t maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);

    int32_t x0 = (minx + 15) >> 4;
    int32_t y0 = (miny + 15) >> 4;
    int32_t x1 = maxx >> 4;
    int32_t y1 = maxy >> 4;

    if (x0 < s->scissor_x) x0 = s->scissor_x;
    if (y0 < s->scissor_y) y0 = s->scissor_y;
    if (x1 >= s->scissor_x + s->scissor_w) x1 = s->scissor_x + s->scissor_w - 1;
    if (y1 >= s->scissor_y + s->scissor_h) y1 = s->scissor_y + s->scissor_h - 1;
    if (x0 > x1 || y0 > y1) {
        return;
    }

    /* 边缘函数在像素中心处的值，按行/列增量推进 */
    const int32_t px0 = x0 * 16 + 8;
    const int32_t py0 = y0 * 16 + 8;

    int32_t w0_row = (int32_t)(c.x - b.x) * (py0 - b.y) - (int32_t)(c.y - b.y) * (px0 - b.x);
    int32_t w1_row = (int32_t)(a.x - c.x) * (py0 - c.y) - (int32_t)(a.y - c.y) * (px0 - c.x);
    int32_t w2_row = (int32_t)(b.x - a.x) * (py0 - a.y) - (int32_t)(b.y - a.y) * (px0 - a.x);

    const int32_t dw0dx = -(c.y - b.y) * 16;
    const int32_t dw0dy = (c.x - b.x) * 16;
    const int32_t dw1dx = -(a.y - c.y) * 16;
    const int32_t dw1dy = (a.x - c.x) * 16;
    const int32_t dw2dx = -(b.y - a.y) * 16;
    const int32_t dw2dy = (b.x - a.x) * 16;

    /* UV：以 texel 为单位，按像素步进 */
    const float inv_area = 1.0f / (float)area;
    const float ua = a.u / 16.0f, va = a.v / 16.0f;
    const float ub = b.u / 16.0f, vb = b.v / 16.0f;
    const float uc = c.u / 16.0f, vc = c.v / 16.0f;

    const float du_dx = (ua * (float)(b.y - c.y) + ub * (float)(c.y - a.y) +
                         uc * (float)(a.y - b.y)) * inv_area * 16.0f;
    const float dv_dx = (va * (float)(b.y - c.y) + vb * (float)(c.y - a.y) +
                         vc * (float)(a.y - b.y)) * inv_area * 16.0f;
    const float du_dy = (ua * (float)(c.x - b.x) + ub * (float)(a.x - c.x) +
                         uc * (float)(b.x - a.x)) * inv_area * 16.0f;
    const float dv_dy = (va * (float)(c.x - b.x) + vb * (float)(a.x - c.x) +
                         vc * (float)(b.x - a.x)) * inv_area * 16.0f;

    /* 首行首列的 UV */
    float u_row = (ua * (float)w0_row + ub * (float)w1_row + uc * (float)w2_row) * inv_area;
    float v_row = (va * (float)w0_row + vb * (float)w1_row + vc * (float)w2_row) * inv_area;

    /* 顶点色调制是常量：把解包提到循环外。
     * 之前每个像素都做一遍 unpack565×2 + pack565，实测这是内层循环的大头。 */
    const bool modulate = (flat_color != 0xFFFFu);
    uint32_t tr = 31, tg = 63, tb = 31;
    if (modulate) {
        unpack565(flat_color, &tr, &tg, &tb);
    }

    /* 把状态和纹理字段提到局部变量。
     * 内层循环里有 row[x] = src 这样的写入，编译器无法证明 st/tex 不被别名，
     * 于是每个像素都要重新加载 filter/wrap/blend/width/height —— 这是主要的
     * 隐藏开销。 */
    const uint8_t blend = st->blend;
    const uint8_t filter = st->filter;
    const uint8_t wrap = st->wrap;
    const uint8_t alpha_test = st->alpha_test;
    const uint8_t alpha_ref = st->alpha_ref;
    const uint16_t *restrict tpx = tex ? tex->pixels : NULL;

    for (int32_t y = y0; y <= y1; y++) {
        int32_t w0 = w0_row;
        int32_t w1 = w1_row;
        int32_t w2 = w2_row;
        float u = u_row;
        float v = v_row;

        uint16_t *row = s->pixels + (size_t)y * s->width;

        for (int32_t x = x0; x <= x1; x++) {
            if ((w0 | w1 | w2) >= 0) {
                uint8_t alpha = 255;
                uint16_t src;
                if (tpx != NULL) {
                    src = tex_sample(tex, u, v, filter, wrap, &alpha);
                    if (modulate) {
                        uint32_t sr, sg, sb;
                        unpack565(src, &sr, &sg, &sb);
                        src = pack565(sr * tr / 31, sg * tg / 63, sb * tb / 31);
                    }
                } else {
                    src = flat_color;
                    alpha = 255;
                }

                if (alpha_test && alpha < alpha_ref) {
                    /* 丢弃 */
                } else if (blend == ESPGPU_BLEND_NONE || alpha == 255) {
                    row[x] = src;
                } else {
                    row[x] = raster_blend_rgb565(src, row[x], alpha, blend);
                }
            }
            w0 += dw0dx;
            w1 += dw1dx;
            w2 += dw2dx;
            u += du_dx;
            v += dv_dx;
        }

        w0_row += dw0dy;
        w1_row += dw1dy;
        w2_row += dw2dy;
        u_row += du_dy;
        v_row += dv_dy;
    }
}

IRAM_ATTR void raster_draw_quad(espgpu_surface_t *s, const espgpu_texture_t *tex,
                      const espgpu_vertex2d_t v[4], const espgpu_draw_state_t *st)
{
    const uint16_t flat = v[0].color;
    draw_tri(s, tex, v[0], v[1], v[2], st, flat);
    draw_tri(s, tex, v[0], v[2], v[3], st, flat);
}
