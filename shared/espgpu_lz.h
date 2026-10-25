/*
 * espgpu_lz.h — 下行命令流压缩（LZ4 block 兼容格式）
 *
 * PLAN §2 的带宽账本里，下行命令流按 heatshrink 估了 12–18 KB/帧。
 * 这里用 LZ4 block 格式：解码器只有几十行、不需要额外内存，S3 上解 24 KB
 * 不到 1 ms；编码器在主机端，贪心哈希匹配。
 *
 * chunk 载荷（flags 带 ESPGPU_F_LZ）= u32 解压后长度 + LZ4 block。
 *
 * 纯头文件，固件与主机共用。所有函数都做边界检查，坏数据只会返回失败。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPGPU_LZ_MIN_MATCH 4u
#define ESPGPU_LZ_HASH_BITS 12u
#define ESPGPU_LZ_MAX_OFFSET 65535u

/** 最坏情况输出上界 */
static inline size_t espgpu_lz_bound(size_t n)
{
    return n + n / 255u + 16u;
}

/**
 * 解码。成功返回写入 dst 的字节数，失败返回 0。
 * 输出必须恰好等于 dst_len 才算成功（由调用方的长度前缀给出）。
 */
static inline size_t espgpu_lz_decode(const uint8_t *src, size_t src_len, uint8_t *dst,
                                      size_t dst_len)
{
    size_t si = 0;
    size_t di = 0;

    while (si < src_len) {
        const uint8_t token = src[si++];

        /* 字面量 */
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (si >= src_len) {
                    return 0;
                }
                b = src[si++];
                lit += b;
            } while (b == 255);
        }
        if (lit > src_len - si || lit > dst_len - di) {
            return 0;
        }
        memcpy(dst + di, src + si, lit);
        si += lit;
        di += lit;

        if (si >= src_len) {
            break; /* 最后一个序列只有字面量 */
        }

        /* 匹配 */
        if (src_len - si < 2) {
            return 0;
        }
        const size_t off = (size_t)src[si] | ((size_t)src[si + 1] << 8);
        si += 2;
        if (off == 0 || off > di) {
            return 0;
        }

        size_t mlen = (token & 0x0F);
        if (mlen == 15) {
            uint8_t b;
            do {
                if (si >= src_len) {
                    return 0;
                }
                b = src[si++];
                mlen += b;
            } while (b == 255);
        }
        mlen += ESPGPU_LZ_MIN_MATCH;
        if (mlen > dst_len - di) {
            return 0;
        }

        /* 不重叠（off >= mlen）时 memcpy；重叠（off < mlen，即游程）必须逐字节才符合 LZ4 语义。
         * 很短的匹配逐字节反而比函数调用便宜，所以只有较长时才走 memcpy */
        const uint8_t *m = dst + di - off;
        if (off >= mlen && mlen >= 16) {
            memcpy(dst + di, m, mlen);
        } else {
            for (size_t k = 0; k < mlen; k++) {
                dst[di + k] = m[k];
            }
        }
        di += mlen;
    }
    return di == dst_len ? di : 0;
}

static inline uint32_t espgpu_lz_read32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline size_t espgpu_lz_put_len(uint8_t *dst, size_t o, size_t cap, size_t len)
{
    while (len >= 255) {
        if (o >= cap) {
            return (size_t)-1;
        }
        dst[o++] = 255;
        len -= 255;
    }
    if (o >= cap) {
        return (size_t)-1;
    }
    dst[o++] = (uint8_t)len;
    return o;
}

/**
 * 编码。返回压缩后字节数；输出缓冲不够或压缩无收益时返回 0（调用方发原文）。
 * table 由调用方提供，至少 (1 << ESPGPU_LZ_HASH_BITS) 个 uint32_t，避免在栈上开 16 KB。
 */
static inline size_t espgpu_lz_encode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap,
                                      uint32_t *table)
{
    const size_t hsize = (size_t)1 << ESPGPU_LZ_HASH_BITS;
    for (size_t i = 0; i < hsize; i++) {
        table[i] = 0xFFFFFFFFu;
    }

    size_t o = 0;
    size_t anchor = 0;
    size_t i = 0;

    /* 末尾 12 字节内不再找匹配（LZ4 惯例，保证最后一个序列是纯字面量） */
    const size_t limit = n > 12 ? n - 12 : 0;

    while (i < limit) {
        const uint32_t seq = espgpu_lz_read32(src + i);
        const uint32_t h = (seq * 2654435761u) >> (32 - ESPGPU_LZ_HASH_BITS);
        const uint32_t cand = table[h];
        table[h] = (uint32_t)i;

        if (cand == 0xFFFFFFFFu || i - cand > ESPGPU_LZ_MAX_OFFSET ||
            espgpu_lz_read32(src + cand) != seq) {
            i++;
            continue;
        }

        /* 延长匹配 */
        size_t mlen = ESPGPU_LZ_MIN_MATCH;
        const size_t max_m = n - 5 - i; /* 留至少 5 字节给末尾字面量 */
        while (mlen < max_m && src[cand + mlen] == src[i + mlen]) {
            mlen++;
        }

        /* 写序列：token + 字面量长度扩展 + 字面量 + offset + 匹配长度扩展 */
        const size_t lit = i - anchor;
        const size_t ml = mlen - ESPGPU_LZ_MIN_MATCH;
        if (o >= cap) {
            return 0;
        }
        const size_t tok_pos = o++;
        uint8_t token = (uint8_t)(((lit >= 15 ? 15 : lit) << 4) | (ml >= 15 ? 15 : ml));
        if (lit >= 15) {
            o = espgpu_lz_put_len(dst, o, cap, lit - 15);
            if (o == (size_t)-1) {
                return 0;
            }
        }
        if (o + lit + 2 > cap) {
            return 0;
        }
        memcpy(dst + o, src + anchor, lit);
        o += lit;
        const size_t off = i - cand;
        dst[o++] = (uint8_t)(off & 0xFF);
        dst[o++] = (uint8_t)(off >> 8);
        if (ml >= 15) {
            o = espgpu_lz_put_len(dst, o, cap, ml - 15);
            if (o == (size_t)-1) {
                return 0;
            }
        }
        dst[tok_pos] = token;

        i += mlen;
        anchor = i;
    }

    /* 末尾字面量 */
    const size_t lit = n - anchor;
    if (o >= cap) {
        return 0;
    }
    dst[o++] = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
    if (lit >= 15) {
        o = espgpu_lz_put_len(dst, o, cap, lit - 15);
        if (o == (size_t)-1) {
            return 0;
        }
    }
    if (o + lit > cap) {
        return 0;
    }
    memcpy(dst + o, src + anchor, lit);
    o += lit;

    return o < n ? o : 0;
}

#ifdef __cplusplus
}
#endif
