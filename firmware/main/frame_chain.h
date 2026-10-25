#pragma once

/*
 * frame_chain.h — 命令模式「差分参考链」的簿记
 *
 * 生产者 = 命令任务（PRESENT 时编码），消费者 = 发送任务（core1）。两者之间有
 * SEND_SLOTS 个槽位，所以一帧发送失败时，后面可能已经有基于它编码好的差分帧在排队。
 *
 * 不变式：主机收到的每一个差分帧（DELTA_RLE / 非关键 BLOCK），参考帧都是主机收到的上一帧。
 *   - 生产者每帧都对 shadow（= 上一个「生产出来」的帧）做差分；
 *   - 发送任务一旦有帧没完整发出（超时 / 跳过），链就断了：之后的差分帧参考的
 *     是主机没有的帧，一律不发，直到一个关键帧发送成功；
 *   - 断链时 fail_seq++，生产者看到 fail_seq 变化就出关键帧。
 *
 * fail_seq 只有发送任务写，fail_seen 只有生产者写：单写者 + 32 位对齐读写，
 * 不需要原子操作（以前是两边都写 force_keyframe，发送任务置的 true 会被
 * 命令任务编码完成后的 false 覆盖掉）。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espgpu_proto.h"

/** 周期性兜底关键帧 */
#define CHAIN_KEY_INTERVAL 60u

/* ── AUTO 编码策略（ESPGPU_CODEC_AUTO）的参数 ── */
/** USB FS 实测上行 ~0.75 MB/s（bench-results），留一点余量算每帧预算 */
#define CHAIN_AUTO_LINK_BPS 720000u
/** 静止判定：一帧里变化的块 <= 总块数 / CHAIN_AUTO_CALM_DIV */
#define CHAIN_AUTO_CALM_DIV 100u
/** 连续这么多帧静止后补一帧无损关键帧 */
#define CHAIN_AUTO_CALM_FRAMES 3u
/** 无损刷新太大（> 预算 x 这个倍数）就放弃，并冷却一段时间再试 */
#define CHAIN_AUTO_REFRESH_SLACK 4u
#define CHAIN_AUTO_REFRESH_BACKOFF 120u

typedef struct {
    volatile uint32_t fail_seq; /**< 发送任务写：断链次数 */
    bool broken;                /**< 发送任务私有：当前链是否已断 */
} chain_tx_t;

typedef struct {
    uint32_t fail_seen;        /**< 生产者私有：已经响应过的 fail_seq */
    uint32_t frames_since_key; /**< 距上一个关键帧的帧数 */
    uint32_t last_delta_bytes; /**< 上一帧 DELTA_RLE 大小，用于自适应选 RLE */
    /* AUTO 状态 */
    bool lossy;                /**< 主机现在的画面是有损的（当前链从 BLOCK 关键帧开始） */
    uint32_t calm;             /**< 连续"几乎没变"的帧数 */
    uint32_t refresh_backoff;  /**< 无损刷新失败后的冷却帧数 */
} chain_prod_t;

/**
 * 生产者每帧开头调用：这一帧是否必须是关键帧。
 * force = 外部要求（模式变更 / 主机 REQUEST_KEYFRAME / 上一帧编码失败）。
 * 调用之后如果这一帧最终没有产出（编码失败），调用者必须重新请求关键帧。
 */
static inline bool chain_need_key(chain_prod_t *p, const chain_tx_t *tx, bool force)
{
    const uint32_t fs = tx->fail_seq;
    const bool resync = fs != p->fail_seen;
    p->fail_seen = fs;
    return force || resync || p->frames_since_key >= CHAIN_KEY_INTERVAL;
}

/**
 * DELTA_RLE 模式的选择：关键帧时发 RLE；上一帧差分效果很差（内容整屏在变，
 * 这时 RLE 更小 —— bench-results 里 bars 图案 10.2x vs 5.3x 的结论）也发 RLE。
 */
static inline uint8_t chain_pick_delta_rle(const chain_prod_t *p, bool need_key, size_t raw)
{
    return (need_key || p->last_delta_bytes > raw / 3) ? ESPGPU_CODEC_RLE
                                                       : ESPGPU_CODEC_DELTA_RLE;
}

/** 生产者：这一帧已编码并排队（shadow 已更新为这一帧）。 */
static inline void chain_produced(chain_prod_t *p, bool key, uint8_t codec, uint32_t enc_bytes)
{
    if (key) {
        p->frames_since_key = 0;
        p->last_delta_bytes = 0;
        p->lossy = codec == ESPGPU_CODEC_BLOCK;
    } else {
        p->frames_since_key++;
        p->last_delta_bytes = codec == ESPGPU_CODEC_DELTA_RLE ? enc_bytes : 0;
    }
}

/** 发送任务：这一帧能不能发（断链时差分帧的参考帧主机没有）。 */
static inline bool chain_tx_should_send(const chain_tx_t *tx, bool key)
{
    return !(tx->broken && !key);
}

/** 发送任务：一帧发送的结果（被 chain_tx_should_send 跳过的帧不调用）。 */
static inline void chain_tx_done(chain_tx_t *tx, bool key, bool ok)
{
    if (ok) {
        if (key) {
            tx->broken = false; /* 关键帧到了，主机参考帧重新对齐 */
        }
    } else {
        tx->broken = true;
        tx->fail_seq = tx->fail_seq + 1;
    }
}
