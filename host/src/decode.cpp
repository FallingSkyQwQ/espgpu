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
 *
 * 参考帧就是上一帧的解码结果：RGB565 直接用 dev->rgb565，INDEX8 / R8 用 dev->prev。
 * 不变式：ref_valid 为真时，rgb565 / prev / rgba 三者都对应同一帧（上一帧）。
 * 所以差分帧可以原地应用，只改 XOR 非零的像素（静止画面几乎零开销）。
 * 任何一帧解码失败或丢失，ref_valid 置假，之后的差分帧一律丢弃并要求关键帧，
 * 而不是把差分叠在错误的参考帧上显示残影。
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

/* RGB565 -> RGBA8888（内存字节序 R,G,B,A），按位复制扩展到 8 位 */
inline uint32_t to_rgba(uint32_t v)
{
    const uint32_t r5 = (v >> 11) & 0x1F;
    const uint32_t g6 = (v >> 5) & 0x3F;
    const uint32_t b5 = v & 0x1F;
    const uint32_t r = (r5 << 3) | (r5 >> 2);
    const uint32_t g = (g6 << 2) | (g6 >> 4);
    const uint32_t b = (b5 << 3) | (b5 >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

inline void st32(uint8_t *p, uint32_t v)
{
    /* RGBA 缓冲按小端 u32 写：Windows / x86 / ARM64 都是小端 */
    memcpy(p, &v, 4);
}

/** 只做语法检查：总像素数必须恰好等于 pixels，且不越界。比真正解码便宜得多。 */
bool rle_scan(const uint8_t *src, size_t src_len, size_t pixels, unsigned bpp)
{
    size_t si = 0;
    size_t di = 0;
    while (si < src_len) {
        const uint8_t c = src[si++];
        size_t n;
        size_t bytes;
        if (c & 0x80) {
            n = static_cast<size_t>(c & 0x7F) + 2;
            bytes = bpp;
        } else {
            n = static_cast<size_t>(c) + 1;
            bytes = n * bpp;
        }
        if (bytes > src_len - si || n > pixels - di) {
            log_msg(LOG_WARN, "rle: overflow si=%zu/%zu px=%zu/%zu", si, src_len, di, pixels);
            return false;
        }
        si += bytes;
        di += n;
    }
    if (di != pixels) {
        log_msg(LOG_WARN, "rle: length mismatch produced=%zu expected=%zu", di, pixels);
        return false;
    }
    return true;
}

/** RLE 解码（调用前已经 rle_scan 过，这里不再做边界检查）。 */
void rle_decode16(const uint8_t *src, size_t src_len, uint16_t *dst)
{
    size_t si = 0;
    size_t di = 0;
    while (si < src_len) {
        const uint8_t c = src[si++];
        if (c & 0x80) {
            const size_t run = static_cast<size_t>(c & 0x7F) + 2;
            const uint16_t v = rd16(src + si);
            si += 2;
            for (size_t k = 0; k < run; k++) {
                dst[di + k] = v;
            }
            di += run;
        } else {
            const size_t n = static_cast<size_t>(c) + 1;
            memcpy(dst + di, src + si, n * 2);
            si += n * 2;
            di += n;
        }
    }
}

void rle_decode8(const uint8_t *src, size_t src_len, uint8_t *dst)
{
    size_t si = 0;
    size_t di = 0;
    while (si < src_len) {
        const uint8_t c = src[si++];
        if (c & 0x80) {
            const size_t run = static_cast<size_t>(c & 0x7F) + 2;
            memset(dst + di, src[si], run);
            si += 1;
            di += run;
        } else {
            const size_t n = static_cast<size_t>(c) + 1;
            memcpy(dst + di, src + si, n);
            si += n;
            di += n;
        }
    }
}

/** 差分原地应用（RGB565）：XOR 为 0 的游程直接跳过。同步更新 rgba。 */
void delta_apply16(const uint8_t *src, size_t src_len, uint16_t *px, uint8_t *rgba)
{
    size_t si = 0;
    size_t di = 0;
    while (si < src_len) {
        const uint8_t c = src[si++];
        if (c & 0x80) {
            const size_t run = static_cast<size_t>(c & 0x7F) + 2;
            const uint16_t x = rd16(src + si);
            si += 2;
            if (x != 0) {
                for (size_t k = di; k < di + run; k++) {
                    const uint16_t v = static_cast<uint16_t>(px[k] ^ x);
                    px[k] = v;
                    st32(rgba + k * 4, to_rgba(v));
                }
            }
            di += run;
        } else {
            const size_t n = static_cast<size_t>(c) + 1;
            for (size_t k = 0; k < n; k++) {
                const uint16_t v = static_cast<uint16_t>(px[di + k] ^ rd16(src + si + k * 2));
                px[di + k] = v;
                st32(rgba + (di + k) * 4, to_rgba(v));
            }
            si += n * 2;
            di += n;
        }
    }
}

/** 差分原地应用（INDEX8 / R8）。lut 为 CLUT 展开后的 RGBA。 */
void delta_apply8(const uint8_t *src, size_t src_len, uint8_t *idx, uint16_t *px565,
                  uint8_t *rgba, const uint16_t *clut, const uint32_t *lut, bool convert)
{
    size_t si = 0;
    size_t di = 0;
    while (si < src_len) {
        const uint8_t c = src[si++];
        size_t n;
        if (c & 0x80) {
            n = static_cast<size_t>(c & 0x7F) + 2;
            const uint8_t x = src[si++];
            if (x != 0) {
                for (size_t k = di; k < di + n; k++) {
                    const uint8_t v = static_cast<uint8_t>(idx[k] ^ x);
                    idx[k] = v;
                    if (convert) {
                        px565[k] = clut[v];
                        st32(rgba + k * 4, lut[v]);
                    }
                }
            }
        } else {
            n = static_cast<size_t>(c) + 1;
            for (size_t k = 0; k < n; k++) {
                const uint8_t v = static_cast<uint8_t>(idx[di + k] ^ src[si + k]);
                idx[di + k] = v;
                if (convert) {
                    px565[di + k] = clut[v];
                    st32(rgba + (di + k) * 4, lut[v]);
                }
            }
            si += n;
        }
        di += n;
    }
}

void convert16(const uint16_t *px, size_t pixels, uint8_t *rgba)
{
    for (size_t i = 0; i < pixels; i++) {
        st32(rgba + i * 4, to_rgba(px[i]));
    }
}

void convert8(const uint8_t *idx, size_t pixels, uint16_t *px565, uint8_t *rgba,
              const uint16_t *clut, const uint32_t *lut)
{
    for (size_t i = 0; i < pixels; i++) {
        px565[i] = clut[idx[i]];
        st32(rgba + i * 4, lut[idx[i]]);
    }
}

} // namespace

int frame_decode(espgpu_device *dev, const uint8_t *enc, size_t enc_len, uint32_t raw_len,
                 int w, int h, uint8_t format, uint8_t codec)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        log_msg(LOG_WARN, "bad frame size %dx%d", w, h);
        return -1;
    }

    const unsigned bpp = (format == ESPGPU_FMT_INDEX8 || format == ESPGPU_FMT_R8) ? 1u : 2u;
    const size_t pixels = static_cast<size_t>(w) * h;
    const size_t need = pixels * bpp;
    if (raw_len != need) {
        log_msg(LOG_WARN, "raw_len %u != %zu for %dx%d bpp=%u", raw_len, need, w, h, bpp);
        return -1;
    }
    if (codec != ESPGPU_CODEC_RAW && codec != ESPGPU_CODEC_RLE &&
        codec != ESPGPU_CODEC_DELTA_RLE) {
        log_msg(LOG_WARN, "unsupported codec %u", codec);
        return -1;
    }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    std::lock_guard<std::mutex> lk(dev->frame_mu);

    const bool delta = codec == ESPGPU_CODEC_DELTA_RLE;
    if (delta && !(dev->ref_valid && dev->width == w && dev->height == h &&
                   dev->format == format)) {
        /* 没有可信的参考帧（刚打开 / 丢过帧 / 换过模式）：丢掉，等关键帧 */
        std::lock_guard<std::mutex> slk(dev->stats_mu);
        dev->stats.frames_dropped++;
        return -1;
    }

    const bool ok = codec == ESPGPU_CODEC_RAW ? enc_len >= need
                                              : rle_scan(enc, enc_len, pixels, bpp);
    if (!ok) {
        log_msg(LOG_WARN, "frame stream malformed (codec=%u enc=%zu raw=%zu)", codec, enc_len,
                need);
        /* 设备的参考帧已经是这一帧了，主机这边对不上：后续差分全部作废 */
        dev->ref_valid = false;
        return -1;
    }

    if (bpp == 1 && (dev->clut_dirty || !delta)) {
        for (int i = 0; i < 256; i++) {
            dev->clut_rgba[i] = to_rgba(dev->clut[i]);
        }
    }

    if (!delta) {
        dev->rgb565.resize(pixels);
        dev->rgba.resize(pixels * 4);
        if (bpp == 2) {
            if (codec == ESPGPU_CODEC_RAW) {
                memcpy(dev->rgb565.data(), enc, need);
            } else {
                rle_decode16(enc, enc_len, dev->rgb565.data());
            }
            convert16(dev->rgb565.data(), pixels, dev->rgba.data());
            dev->prev.clear();
        } else {
            dev->prev.resize(pixels);
            if (codec == ESPGPU_CODEC_RAW) {
                memcpy(dev->prev.data(), enc, need);
            } else {
                rle_decode8(enc, enc_len, dev->prev.data());
            }
            convert8(dev->prev.data(), pixels, dev->rgb565.data(), dev->rgba.data(), dev->clut,
                     dev->clut_rgba);
        }
    } else if (bpp == 2) {
        delta_apply16(enc, enc_len, dev->rgb565.data(), dev->rgba.data());
    } else {
        /* CLUT 变了：未改动的像素也得重新查表，那就整帧转换一遍 */
        const bool full = dev->clut_dirty;
        delta_apply8(enc, enc_len, dev->prev.data(), dev->rgb565.data(), dev->rgba.data(),
                     dev->clut, dev->clut_rgba, !full);
        if (full) {
            convert8(dev->prev.data(), pixels, dev->rgb565.data(), dev->rgba.data(), dev->clut,
                     dev->clut_rgba);
        }
    }
    if (bpp == 1) {
        dev->clut_dirty = false;
    }

    dev->width = w;
    dev->height = h;
    dev->format = format;
    dev->codec = codec;
    dev->ref_valid = true;

    dev->frame_id = dev->cur_frame_id;
    dev->frame_ready = true;
    dev->frame_cv.notify_all();

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
