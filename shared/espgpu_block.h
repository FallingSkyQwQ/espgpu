/*
 * espgpu_block.h — 有损块编码 ESPGPU_CODEC_BLOCK（BC1 / DXT1 块 + 块级差分）
 *
 * 为什么需要：USB Full-Speed 实测只有 ~0.75 MB/s，24 fps 时每帧预算 ~31 KB。
 * 320x200 RGB565 一帧 128 KB，镜头一动整屏都变（3D、Doom 走动），带纹理的画面
 * 用无损 RLE 只能压到 1.0–1.5 倍，于是 USB 一帧要 100–180 ms（issue #2）。
 * BC1 固定 4:1（每 4x4 块 8 字节，0.5 B/px），320x200 整帧最多 32 KB；
 * 没变的块直接跳过，静止画面只剩一张位图（500 B）。
 *
 * 载荷格式（帧头 codec = ESPGPU_CODEC_BLOCK，format = RGB565）：
 *
 *   bitmap   ceil(nblocks / 8) 字节，块按行主序编号，bit i（第 i>>3 字节的第 i&7 位）= 块 i 有数据
 *   blocks   每个置位的块 8 字节，按编号顺序：
 *              u16 c0, u16 c1（RGB565，小端），u32 索引（像素 (x,y) 在 bit 2*(y*4+x)）
 *            总是 4 色模式（c0 > c1）：0 = c0, 1 = c1, 2 = (2c0+c1)/3, 3 = (c0+2c1)/3；
 *            c0 == c1 时所有索引为 0（纯色块）。
 *
 *   nblocks = ceil(w/4) * ceil(h/4)。宽高不是 4 的倍数时，越界像素在编码时取边缘像素、
 *   解码时丢弃。
 *
 * 关键帧 = 位图全置位。差分帧只带变化的块，主机必须有可信的上一帧（与 DELTA_RLE 相同的
 * 参考帧规则）。"变化" 按设备上的**源图**比较（shadow = 上一帧的原始像素），所以有损误差
 * 不会累积：没变的源块在主机那边也还是同一个 BC1 解码结果。
 *
 * 纯头文件，固件、主机解码、软件模拟器共用；只用整数运算。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t espgpu_block_count(uint32_t w, uint32_t h)
{
    return ((w + 3u) / 4u) * ((h + 3u) / 4u);
}

static inline uint32_t espgpu_block_bitmap_bytes(uint32_t w, uint32_t h)
{
    return (espgpu_block_count(w, h) + 7u) / 8u;
}

/** 编码输出上界（关键帧） */
static inline size_t espgpu_block_max_encoded(uint32_t w, uint32_t h)
{
    return (size_t)espgpu_block_bitmap_bytes(w, h) + (size_t)espgpu_block_count(w, h) * 8u;
}

/* ── BC1 单块编码 ─────────────────────────────────────────────────── */

/**
 * 16 个 RGB565 像素 → 8 字节 BC1。端点取包围盒对角，按协方差符号选对角方向，
 * 再向内收 1/16 减少量化误差（stb_dxt 的做法）；索引按投影就近取。
 */
static inline void espgpu_bc1_encode_block(const uint16_t px[16], uint8_t out[8])
{
    int r[16], g[16], b[16];
    int minr = 255, ming = 255, minb = 255, maxr = 0, maxg = 0, maxb = 0;
    int sr = 0, sg = 0, sb = 0;
    int same = 1;
    for (int i = 0; i < 16; i++) {
        const uint16_t c = px[i];
        same &= (c == px[0]);
        /* 展开到 8 位 */
        const int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
        r[i] = (r5 << 3) | (r5 >> 2);
        g[i] = (g6 << 2) | (g6 >> 4);
        b[i] = (b5 << 3) | (b5 >> 2);
        sr += r[i];
        sg += g[i];
        sb += b[i];
        if (r[i] < minr) minr = r[i];
        if (r[i] > maxr) maxr = r[i];
        if (g[i] < ming) ming = g[i];
        if (g[i] > maxg) maxg = g[i];
        if (b[i] < minb) minb = b[i];
        if (b[i] > maxb) maxb = b[i];
    }
    if (same) {
        out[0] = (uint8_t)(px[0] & 0xFF);
        out[1] = (uint8_t)(px[0] >> 8);
        out[2] = out[0];
        out[3] = out[1];
        out[4] = out[5] = out[6] = out[7] = 0;
        return;
    }

    /* 协方差符号：r、b 与 g 负相关时翻转该通道的对角方向 */
    const int mr = sr >> 4, mg = sg >> 4, mb = sb >> 4;
    int crg = 0, cbg = 0;
    for (int i = 0; i < 16; i++) {
        crg += (r[i] - mr) * (g[i] - mg);
        cbg += (b[i] - mb) * (g[i] - mg);
    }
    /* 以变化最大的通道为主轴来定方向：g 范围很小时改用 r 做参照 */
    int a0r = maxr, a1r = minr, a0g = maxg, a1g = ming, a0b = maxb, a1b = minb;
    if (maxg - ming < 8 && maxr - minr >= maxb - minb) {
        int cbr = 0;
        for (int i = 0; i < 16; i++) {
            cbr += (b[i] - mb) * (r[i] - mr);
        }
        if (cbr < 0) {
            a0b = minb;
            a1b = maxb;
        }
    } else {
        if (crg < 0) {
            a0r = minr;
            a1r = maxr;
        }
        if (cbg < 0) {
            a0b = minb;
            a1b = maxb;
        }
    }

    /* 内收 1/16 */
    {
        const int ir = (a0r - a1r) / 16, ig = (a0g - a1g) / 16, ib = (a0b - a1b) / 16;
        a0r -= ir;
        a1r += ir;
        a0g -= ig;
        a1g += ig;
        a0b -= ib;
        a1b += ib;
    }

    uint16_t c0 = (uint16_t)(((a0r >> 3) << 11) | ((a0g >> 2) << 5) | (a0b >> 3));
    uint16_t c1 = (uint16_t)(((a1r >> 3) << 11) | ((a1g >> 2) << 5) | (a1b >> 3));
    if (c0 < c1) {
        const uint16_t t = c0;
        c0 = c1;
        c1 = t;
    }

    uint32_t idx = 0;
    if (c0 != c1) {
        /* 用量化后的端点（与解码器完全一致的颜色）定轴 */
        int pr[2], pg[2], pb[2];
        const int r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
        const int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
        pr[0] = (r0 << 3) | (r0 >> 2);
        pg[0] = (g0 << 2) | (g0 >> 4);
        pb[0] = (b0 << 3) | (b0 >> 2);
        pr[1] = (r1 << 3) | (r1 >> 2);
        pg[1] = (g1 << 2) | (g1 >> 4);
        pb[1] = (b1 << 3) | (b1 >> 2);
        /* 投影到 c1→c0 轴上按 1/3 分段取最近：每像素 3 乘 + 3 比较，
         * 比逐色求距离（16x4 次）快 4 倍；调色板四色本来就在这条轴上 */
        const int ar = pr[0] - pr[1], ag = pg[0] - pg[1], ab = pb[0] - pb[1];
        const int L = ar * ar + ag * ag + ab * ab;
        static const uint8_t k_lvl2idx[4] = {1, 3, 2, 0};
        uint8_t lv[16];
        for (int i = 0; i < 16; i++) {
            const int t6 = 6 * ((r[i] - pr[1]) * ar + (g[i] - pg[1]) * ag + (b[i] - pb[1]) * ab);
            lv[i] = (uint8_t)((t6 >= L) + (t6 >= 3 * L) + (t6 >= 5 * L));
            idx |= (uint32_t)k_lvl2idx[lv[i]] << (2 * i);
        }

        /* 一次最小二乘修正端点（已知每个像素落在轴上的 0/1/2/3 档）：
         * 求 e0 / e1 使 Σ|p - (a·e0 + (1-a)·e1)|² 最小，a = 档/3。
         * 实测比包围盒端点高 ~2 dB PSNR，编码耗时约 1.8 倍。全整数。 */
        int aa = 0, bb = 0, abv = 0;
        int ax[3] = {0, 0, 0}, bx[3] = {0, 0, 0};
        for (int i = 0; i < 16; i++) {
            const int a = lv[i], bw = 3 - lv[i];
            aa += a * a;
            bb += bw * bw;
            abv += a * bw;
            ax[0] += a * r[i];
            ax[1] += a * g[i];
            ax[2] += a * b[i];
            bx[0] += bw * r[i];
            bx[1] += bw * g[i];
            bx[2] += bw * b[i];
        }
        const int det = aa * bb - abv * abv;
        if (det != 0) {
            int e0[3], e1[3];
            for (int k = 0; k < 3; k++) {
                e0[k] = 3 * (ax[k] * bb - bx[k] * abv) / det;
                e1[k] = 3 * (bx[k] * aa - ax[k] * abv) / det;
                e0[k] = e0[k] < 0 ? 0 : (e0[k] > 255 ? 255 : e0[k]);
                e1[k] = e1[k] < 0 ? 0 : (e1[k] > 255 ? 255 : e1[k]);
            }
            uint16_t n0 = (uint16_t)(((e0[0] >> 3) << 11) | ((e0[1] >> 2) << 5) | (e0[2] >> 3));
            uint16_t n1 = (uint16_t)(((e1[0] >> 3) << 11) | ((e1[1] >> 2) << 5) | (e1[2] >> 3));
            if (n0 < n1) {
                const uint16_t t = n0;
                n0 = n1;
                n1 = t;
            }
            if (n0 != n1) {
                c0 = n0;
                c1 = n1;
                const int q0r = (c0 >> 11) & 31, q0g = (c0 >> 5) & 63, q0b = c0 & 31;
                const int q1r = (c1 >> 11) & 31, q1g = (c1 >> 5) & 63, q1b = c1 & 31;
                pr[0] = (q0r << 3) | (q0r >> 2);
                pg[0] = (q0g << 2) | (q0g >> 4);
                pb[0] = (q0b << 3) | (q0b >> 2);
                pr[1] = (q1r << 3) | (q1r >> 2);
                pg[1] = (q1g << 2) | (q1g >> 4);
                pb[1] = (q1b << 3) | (q1b >> 2);
                const int br2 = pr[0] - pr[1], bg2 = pg[0] - pg[1], bb2 = pb[0] - pb[1];
                const int L2 = br2 * br2 + bg2 * bg2 + bb2 * bb2;
                idx = 0;
                for (int i = 0; i < 16; i++) {
                    const int t6 =
                        6 * ((r[i] - pr[1]) * br2 + (g[i] - pg[1]) * bg2 + (b[i] - pb[1]) * bb2);
                    const int lvl = (t6 >= L2) + (t6 >= 3 * L2) + (t6 >= 5 * L2);
                    idx |= (uint32_t)k_lvl2idx[lvl] << (2 * i);
                }
            }
        }
    }
    out[0] = (uint8_t)(c0 & 0xFF);
    out[1] = (uint8_t)(c0 >> 8);
    out[2] = (uint8_t)(c1 & 0xFF);
    out[3] = (uint8_t)(c1 >> 8);
    out[4] = (uint8_t)(idx & 0xFF);
    out[5] = (uint8_t)((idx >> 8) & 0xFF);
    out[6] = (uint8_t)((idx >> 16) & 0xFF);
    out[7] = (uint8_t)(idx >> 24);
}

/** 8 字节 BC1 → 16 个 RGB565（与编码器的调色板完全一致） */
static inline void espgpu_bc1_decode_block(const uint8_t in[8], uint16_t px[16])
{
    const uint16_t c0 = (uint16_t)(in[0] | (in[1] << 8));
    const uint16_t c1 = (uint16_t)(in[2] | (in[3] << 8));
    const uint32_t idx = (uint32_t)in[4] | ((uint32_t)in[5] << 8) | ((uint32_t)in[6] << 16) |
                         ((uint32_t)in[7] << 24);
    uint16_t pal[4];
    pal[0] = c0;
    pal[1] = c1;
    if (c0 == c1) {
        for (int i = 0; i < 16; i++) {
            px[i] = c0;
        }
        return;
    }
    const int r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
    const int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    const int R0 = (r0 << 3) | (r0 >> 2), G0 = (g0 << 2) | (g0 >> 4), B0 = (b0 << 3) | (b0 >> 2);
    const int R1 = (r1 << 3) | (r1 >> 2), G1 = (g1 << 2) | (g1 >> 4), B1 = (b1 << 3) | (b1 >> 2);
    const int R2 = (2 * R0 + R1) / 3, G2 = (2 * G0 + G1) / 3, B2 = (2 * B0 + B1) / 3;
    const int R3 = (R0 + 2 * R1) / 3, G3 = (G0 + 2 * G1) / 3, B3 = (B0 + 2 * B1) / 3;
    pal[2] = (uint16_t)(((R2 >> 3) << 11) | ((G2 >> 2) << 5) | (B2 >> 3));
    pal[3] = (uint16_t)(((R3 >> 3) << 11) | ((G3 >> 2) << 5) | (B3 >> 3));
    for (int i = 0; i < 16; i++) {
        px[i] = pal[(idx >> (2 * i)) & 3];
    }
}

/* ── 整帧 ─────────────────────────────────────────────────────────── */

/**
 * 编码一帧。shadow = 上一帧的源像素（w*h），会被就地更新成 cur；
 * key 为真（或 shadow 为空）时所有块都输出。返回编码字节数，cap 不足返回 0。
 */
static inline size_t espgpu_block_encode(const uint16_t *cur, uint16_t *shadow, uint32_t w,
                                         uint32_t h, int key, uint8_t *out, size_t cap)
{
    const uint32_t bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
    const uint32_t bmp = espgpu_block_bitmap_bytes(w, h);
    if (cap < bmp) {
        return 0;
    }
    memset(out, 0, bmp);
    size_t o = bmp;
    if (shadow == NULL) {
        key = 1;
    }

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint32_t x0 = bx * 4u, y0 = by * 4u;
            const uint32_t nx = (w - x0) < 4u ? (w - x0) : 4u;
            const uint32_t ny = (h - y0) < 4u ? (h - y0) : 4u;

            int changed = key;
            if (!changed) {
                for (uint32_t y = 0; y < ny && !changed; y++) {
                    const size_t row = (size_t)(y0 + y) * w + x0;
                    changed = memcmp(cur + row, shadow + row, nx * 2u) != 0;
                }
            }
            if (!changed) {
                continue;
            }
            if (o + 8u > cap) {
                return 0;
            }
            uint16_t px[16];
            for (uint32_t y = 0; y < 4u; y++) {
                const uint32_t sy = y0 + (y < ny ? y : ny - 1u);
                for (uint32_t x = 0; x < 4u; x++) {
                    const uint32_t sx = x0 + (x < nx ? x : nx - 1u);
                    px[y * 4u + x] = cur[(size_t)sy * w + sx];
                }
            }
            espgpu_bc1_encode_block(px, out + o);
            o += 8u;
            const uint32_t bi = by * bw + bx;
            out[bi >> 3] |= (uint8_t)(1u << (bi & 7u));
            if (shadow) {
                for (uint32_t y = 0; y < ny; y++) {
                    const size_t row = (size_t)(y0 + y) * w + x0;
                    memcpy(shadow + row, cur + row, nx * 2u);
                }
            }
        }
    }
    return o;
}

/** 校验一帧载荷的长度与位图一致。返回 1 合法；*is_key = 位图是否全置位。 */
static inline int espgpu_block_scan(const uint8_t *enc, size_t len, uint32_t w, uint32_t h,
                                    int *is_key)
{
    const uint32_t n = espgpu_block_count(w, h);
    const uint32_t bmp = (n + 7u) / 8u;
    if (len < bmp) {
        return 0;
    }
    uint32_t set = 0;
    for (uint32_t i = 0; i < n; i++) {
        set += (enc[i >> 3] >> (i & 7u)) & 1u;
    }
    /* 位图里越界的位必须为 0，避免两边理解不一致 */
    for (uint32_t i = n; i < bmp * 8u; i++) {
        if ((enc[i >> 3] >> (i & 7u)) & 1u) {
            return 0;
        }
    }
    if (len != (size_t)bmp + (size_t)set * 8u) {
        return 0;
    }
    if (is_key) {
        *is_key = set == n;
    }
    return 1;
}

/**
 * 把已校验的载荷解到 img（w*h RGB565，未变的块保持原样）。
 * on_block(x0, y0, nx, ny, user) 在每个块写完后回调（主机用它只转换变化的区域），可为 NULL。
 */
typedef void (*espgpu_block_cb)(uint32_t x0, uint32_t y0, uint32_t nx, uint32_t ny, void *user);

static inline void espgpu_block_decode(const uint8_t *enc, uint32_t w, uint32_t h, uint16_t *img,
                                       espgpu_block_cb on_block, void *user)
{
    const uint32_t bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
    const uint8_t *p = enc + espgpu_block_bitmap_bytes(w, h);
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint32_t bi = by * bw + bx;
            if (!((enc[bi >> 3] >> (bi & 7u)) & 1u)) {
                continue;
            }
            uint16_t px[16];
            espgpu_bc1_decode_block(p, px);
            p += 8;
            const uint32_t x0 = bx * 4u, y0 = by * 4u;
            const uint32_t nx = (w - x0) < 4u ? (w - x0) : 4u;
            const uint32_t ny = (h - y0) < 4u ? (h - y0) : 4u;
            for (uint32_t y = 0; y < ny; y++) {
                memcpy(img + (size_t)(y0 + y) * w + x0, px + y * 4u, nx * 2u);
            }
            if (on_block) {
                on_block(x0, y0, nx, ny, user);
            }
        }
    }
}

#ifdef __cplusplus
}
#endif
