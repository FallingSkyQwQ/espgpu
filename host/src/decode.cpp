/*
 * decode.cpp — 帧解码
 *
 * 编码格式必须与 firmware/main/framebuffer.c 的编码器严格一致：
 *
 *   控制字节 c
 *     c & 0x80 : 重复游程，像素数 = (c & 0x7F) + 2，后接 bpp 字节像素值
 *     其它     : 字面游程，像素数 = c + 1，后接 count * bpp 字节
 *
 * 差分模式先解出 XOR 结果，再与上一帧逐像素异或。
 */

#include "internal.h"

#include <chrono>
#include <cstring>

namespace espgpu {

namespace {

inline uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

/* 把 RGB565 展开成 8 位分量 */
inline void expand565(uint16_t v, uint8_t &r, uint8_t &g, uint8_t &b)
{
    const uint32_t r5 = (v >> 11) & 0x1F;
    const uint32_t g6 = (v >> 5) & 0x3F;
    const uint32_t b5 = v & 0x1F;
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

/* RLE 解码。返回 false 表示数据被截断或越界。 */
bool rle_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len,
                unsigned bpp)
{
    size_t si = 0;
    size_t di = 0;

    while (si < src_len) {
        const uint8_t c = src[si++];
        if (c & 0x80) {
            const size_t run = static_cast<size_t>(c & 0x7F) + 2;
            if (si + bpp > src_len) {
                log_msg(LOG_WARN, "rle: truncated run at si=%zu/%zu di=%zu/%zu", si, src_len,
                        di, dst_len);
                return false;
            }
            if (di + run * bpp > dst_len) {
                log_msg(LOG_WARN, "rle: run overflow si=%zu di=%zu run=%zu dst=%zu", si, di,
                        run, dst_len);
                return false;
            }
            if (bpp == 1) {
                memset(dst + di, src[si], run);
            } else {
                const uint8_t lo = src[si];
                const uint8_t hi = src[si + 1];
                for (size_t k = 0; k < run; k++) {
                    dst[di + k * 2] = lo;
                    dst[di + k * 2 + 1] = hi;
                }
            }
            si += bpp;
            di += run * bpp;
        } else {
            const size_t n = static_cast<size_t>(c) + 1;
            const size_t bytes = n * bpp;
            if (si + bytes > src_len || di + bytes > dst_len) {
                log_msg(LOG_WARN, "rle: literal overflow si=%zu/%zu di=%zu/%zu n=%zu dst=%zu",
                        si, src_len, di, dst_len, n, dst_len);
                return false;
            }
            memcpy(dst + di, src + si, bytes);
            si += bytes;
            di += bytes;
        }
    }
    if (di != dst_len) {
        log_msg(LOG_WARN, "rle: length mismatch produced=%zu expected=%zu consumed=%zu/%zu", di,
                dst_len, si, src_len);
        return false;
    }
    return true;
}

} // namespace

int frame_decode(espgpu_device *dev, const uint8_t *enc, size_t enc_len, uint32_t raw_len,
                 int w, int h, uint8_t format, uint8_t codec)
{    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        log_msg(LOG_WARN, "bad frame size %dx%d", w, h);
        return -1;
    }

    const unsigned bpp = (format == ESPGPU_FMT_INDEX8 || format == ESPGPU_FMT_R8) ? 1u : 2u;
    const size_t need = static_cast<size_t>(w) * h * bpp;
    if (raw_len != need) {
        log_msg(LOG_WARN, "raw_len %u != %zu for %dx%d bpp=%u", raw_len, need, w, h, bpp);
        return -1;
    }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    std::lock_guard<std::mutex> lk(dev->frame_mu);

    dev->cur.resize(need);

    if (codec == ESPGPU_CODEC_RAW) {
        if (enc_len < need) {
            return -1;
        }
        memcpy(dev->cur.data(), enc, need);
    } else if (codec == ESPGPU_CODEC_RLE || codec == ESPGPU_CODEC_DELTA_RLE) {
        if (!rle_decode(enc, enc_len, dev->cur.data(), need, bpp)) {
            log_msg(LOG_WARN, "RLE stream malformed (enc=%zu raw=%zu)", enc_len, need);
            return -1;
        }
        if (codec == ESPGPU_CODEC_DELTA_RLE) {
            if (dev->prev.size() != need) {
                /* 参考帧还没建立（第一帧），当作全零 */
                dev->prev.assign(need, 0);
            }
            if (bpp == 1) {
                for (size_t i = 0; i < need; i++) {
                    dev->cur[i] ^= dev->prev[i];
                }
            } else {
                for (size_t i = 0; i < need; i += 2) {
                    const uint16_t v = rd16(dev->cur.data() + i) ^ rd16(dev->prev.data() + i);
                    memcpy(dev->cur.data() + i, &v, 2);
                }
            }
        }
    } else {
        log_msg(LOG_WARN, "unsupported codec %u", codec);
        return -1;
    }

    /* 设备像素 -> RGB565 -> RGBA8888（ImGui 的 DX11 后端要 RGBA 字节序） */
    dev->width = w;
    dev->height = h;
    dev->format = format;
    dev->codec = codec;

    const size_t pixels = static_cast<size_t>(w) * h;
    dev->rgb565.resize(pixels);
    dev->rgba.resize(pixels * 4);

    const uint8_t *src = dev->cur.data();
    uint8_t *rgba = dev->rgba.data();

    for (size_t i = 0; i < pixels; i++) {
        uint16_t v;
        if (bpp == 1) {
            v = dev->clut[src[i]];
        } else {
            v = rd16(src + i * 2);
        }
        dev->rgb565[i] = v;

        uint8_t r, g, b;
        expand565(v, r, g, b);
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = 255;
    }

    dev->prev.swap(dev->cur);
    dev->frame_id = dev->cur_frame_id;
    dev->frame_ready = true;

    const auto t1 = clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    {
        std::lock_guard<std::mutex> slk(dev->stats_mu);
        dev->stats.frames_received++;
        dev->stats.decode_ms = static_cast<float>(ms);
        dev->win_frames++;
    }

    return 0;
}

} // namespace espgpu
