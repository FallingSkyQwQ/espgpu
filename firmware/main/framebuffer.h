#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "espgpu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 双缓冲帧缓冲。
 *
 * 内存布局刻意不在结构体里存裸指针数组以外的状态，便于后续扩展 FBO。
 * 缓冲区分配在 PSRAM（内部 SRAM 要留给 tile 缓冲与 IRAM 热代码）。
 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t format; /* ESPGPU_FMT_* */
    uint8_t bpp;    /* 每像素字节数 */
    uint8_t *buf[2];
    uint8_t cur;    /* 当前绘制索引 */
    size_t size;    /* 单缓冲字节数 */
} espgpu_fb_t;

esp_err_t espgpu_fb_init(espgpu_fb_t *fb, uint16_t w, uint16_t h, uint8_t format);
void espgpu_fb_deinit(espgpu_fb_t *fb);

/** 当前绘制目标。 */
static inline uint8_t *espgpu_fb_draw(espgpu_fb_t *fb)
{
    return fb->buf[fb->cur];
}

/** 上一帧已发送的缓冲（差分编码的参考帧）。 */
static inline uint8_t *espgpu_fb_ref(espgpu_fb_t *fb)
{
    return fb->buf[fb->cur ^ 1];
}

/** 呈现成功后交换双缓冲。未交换时参考帧保持与主机一致。 */
static inline void espgpu_fb_swap(espgpu_fb_t *fb)
{
    fb->cur ^= 1;
}

/* ── 测试图案（M1 用于测量不同内容类型的压缩率） ────────────────── */
/* 图案 ID 定义在 shared/espgpu_proto.h 的 ESPGPU_PATTERN_*，主机端也要用。 */

/** 生成测试图案到当前绘制缓冲。 */
void espgpu_fb_test_pattern(espgpu_fb_t *fb, uint32_t frame, uint8_t pattern);

/** 默认 CLUT（256 色，用于 INDEX8 模式）。 */
void espgpu_fb_default_clut(uint16_t out[256]);

/* ── 编码 ─────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t raw_bytes;   /* 裸数据大小 */
    uint32_t enc_bytes;   /* 编码后大小 */
} espgpu_fb_encode_result_t;

/**
 * 把当前绘制缓冲编码到 out。
 *
 * codec:
 *   ESPGPU_CODEC_RAW        —— 直接拷贝
 *   ESPGPU_CODEC_RLE        —— 像素级 RLE
 *   ESPGPU_CODEC_DELTA_RLE  —— 与参考帧异或后 RLE
 *
 * 返回编码后字节数，0 表示输出缓冲不足。
 */
esp_err_t espgpu_fb_encode(espgpu_fb_t *fb, uint8_t codec, uint8_t *out, size_t out_cap,
                           espgpu_fb_encode_result_t *result);

/**
 * 对任意裸缓冲编码。M2 的光栅器把结果画在内部 SRAM 的 surface 上，
 * 不需要经过 espgpu_fb_t，所以把编码器拆出来单独可用。
 *
 * @param cur   当前帧
 * @param ref   差分参考帧，可为 NULL
 * @param bytes 帧字节数
 * @param bpp   每像素字节数（1 或 2）
 */
esp_err_t espgpu_encode_raw(const uint8_t *cur, const uint8_t *ref, size_t bytes, unsigned bpp,
                            uint8_t codec, uint8_t *out, size_t cap,
                            espgpu_fb_encode_result_t *result);

/**
 * 同 espgpu_encode_raw，但参考帧就地更新：返回 ESP_OK 时 shadow 的内容等于 cur。
 *
 * DELTA_RLE 在编码的同一遍里把变化的像素写回 shadow（省掉单独一遍整帧 memcpy，
 * 未变的区域不写、cache 行不变脏）；其它 codec 编码后整帧拷贝。
 * 失败时 shadow 可能已被部分更新 —— 调用者必须把它当作失效（强制下一帧关键帧）。
 */
esp_err_t espgpu_encode_update(const uint8_t *cur, uint8_t *shadow, size_t bytes, unsigned bpp,
                               uint8_t codec, uint8_t *out, size_t cap,
                               espgpu_fb_encode_result_t *result);

/** 编码后最坏情况的上界，用于分配输出缓冲。 */
size_t espgpu_fb_max_encoded(const espgpu_fb_t *fb);

#ifdef __cplusplus
}
#endif
