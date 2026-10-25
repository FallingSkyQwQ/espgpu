#pragma once

/*
 * frame_chain.h — 命令模式「差分参考链」的簿记
 *
 * 生产者 = 命令任务（PRESENT 时编码），消费者 = 发送任务（core1）。两者之间有
 * SEND_SLOTS 个槽位，所以一帧发送失败时，后面可能已经有基于它编码好的差分帧在排队。
 *
 * 不变式：主机收到的每一个 DELTA_RLE 帧，参考帧都是主机收到的上一帧。
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

typedef struct {
    volatile uint32_t fail_seq; /**< 发送任务写：断链次数 */
    bool broken;                /**< 发送任务私有：当前链是否已断 */
} chain_tx_t;

typedef struct {
    uint32_t fail_seen;        /**< 生产者私有：已经响应过的 fail_seq */
    uint32_t frames_since_key; /**< 距上一个关键帧的帧数 */
    uint32_t last_delta_bytes; /**< 上一帧差分大小，用于自适应选 RLE */
} chain_prod_t;

/**
 * 生产者：选这一帧的 codec。
 * force = 外部要求的关键帧（模式变更 / 主机 REQUEST_KEYFRAME / 上一帧编码失败）。
 * 调用之后如果这一帧最终没有产出（编码失败），调用者必须重新请求关键帧。
 */
static inline uint8_t chain_pick_codec(chain_prod_t *p, const chain_tx_t *tx, uint8_t want,
                                       bool force, size_t raw)
{
    const uint32_t fs = tx->fail_seq;
    const bool resync = fs != p->fail_seen;
    p->fail_seen = fs;
    if (want != ESPGPU_CODEC_DELTA_RLE) {
        return want;
    }
    /* 关键帧：外部要求 / 发送断链 / 周期性兜底 / 上一帧差分效果很差（内容整屏在变，
     * 这时 RLE 更小 —— bench-results 里 bars 图案 10.2x vs 5.3x 的结论） */
    if (force || resync || p->frames_since_key >= CHAIN_KEY_INTERVAL ||
        p->last_delta_bytes > raw / 3) {
        return ESPGPU_CODEC_RLE;
    }
    return ESPGPU_CODEC_DELTA_RLE;
}

/** 生产者：这一帧已编码并排队（shadow 已更新为这一帧）。 */
static inline void chain_produced(chain_prod_t *p, uint8_t codec, uint32_t enc_bytes)
{
    if (codec == ESPGPU_CODEC_DELTA_RLE) {
        p->last_delta_bytes = enc_bytes;
        p->frames_since_key++;
    } else {
        p->last_delta_bytes = 0;
        p->frames_since_key = 0;
    }
}

/** 发送任务：这一帧能不能发（断链时差分帧的参考帧主机没有）。 */
static inline bool chain_tx_should_send(const chain_tx_t *tx, uint8_t codec)
{
    return !(tx->broken && codec == ESPGPU_CODEC_DELTA_RLE);
}

/** 发送任务：一帧发送的结果（被 chain_tx_should_send 跳过的帧不调用）。 */
static inline void chain_tx_done(chain_tx_t *tx, uint8_t codec, bool ok)
{
    if (ok) {
        if (codec != ESPGPU_CODEC_DELTA_RLE) {
            tx->broken = false; /* 关键帧到了，主机参考帧重新对齐 */
        }
    } else {
        tx->broken = true;
        tx->fail_seq = tx->fail_seq + 1;
    }
}
