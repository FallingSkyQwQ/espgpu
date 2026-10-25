#pragma once

/*
 * log_fwd.h — 设备日志转发（LOG_SUBSCRIBE）的环形缓冲与分行逻辑
 *
 * 纯 C、不依赖 IDF，主机端单测直接 include。加锁由调用者负责（gpu.c 用 portMUX）。
 *
 * 为什么按「行」而不是按「vprintf 调用」处理：
 *   - Log V2（IDF v5.4+ 可选）一行日志会拆成多次 vprintf（颜色 / 级别 / 时间戳 / tag /
 *     正文 / 换行各一次），在 vprintf 钩子里按调用判级别会把正文全部判成 INFO；
 *   - 环形缓冲按固定字节数取出时会把一行截成两半，各发一个 LOG chunk。
 * 所以钩子只管原样入队，发送端只发完整的行，按行首字母判级别、去掉 ANSI 颜色。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef LOGFWD_RING
#define LOGFWD_RING 4096u /* 必须是 2 的幂 */
#endif

typedef struct {
    char buf[LOGFWD_RING];
    uint32_t head; /* 自由计数，写端推进 */
    uint32_t tail; /* 自由计数，读端推进；used = head - tail */
} logfwd_ring_t;

static inline uint32_t logfwd_used(const logfwd_ring_t *r)
{
    return r->head - r->tail;
}

/** 整段放得下才写（满了整段丢弃，不留半行）。 */
static inline bool logfwd_push(logfwd_ring_t *r, const char *s, uint32_t n)
{
    if (n > LOGFWD_RING - logfwd_used(r)) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        r->buf[(r->head + i) & (LOGFWD_RING - 1)] = s[i];
    }
    r->head += n;
    return true;
}

/** 拷出最多 cap 字节，不消费。 */
static inline uint32_t logfwd_peek(const logfwd_ring_t *r, char *dst, uint32_t cap)
{
    uint32_t n = logfwd_used(r);
    if (n > cap) {
        n = cap;
    }
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = r->buf[(r->tail + i) & (LOGFWD_RING - 1)];
    }
    return n;
}

static inline void logfwd_consume(logfwd_ring_t *r, uint32_t n)
{
    r->tail += n;
}

/**
 * 可以发出去的前缀长度：到最后一个 '\n'（含）为止；没有换行但已经取满 cap
 * （单行超长）时整段强制发出，否则留着等这一行写完。
 */
static inline uint32_t logfwd_complete_prefix(const char *s, uint32_t n, uint32_t cap)
{
    for (uint32_t i = n; i > 0; i--) {
        if (s[i - 1] == '\n') {
            return i;
        }
    }
    return n >= cap ? n : 0;
}

/** 去掉 ANSI CSI 序列（ESC [ 参数 终止字节 0x40..0x7E）和 '\r'，返回新长度。 */
static inline size_t logfwd_clean(char *dst, const char *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const char ch = src[i];
        if (ch == '\033') {
            i++;
            if (i < n && src[i] == '[') {
                i++;
                while (i < n && !((unsigned char)src[i] >= 0x40 && (unsigned char)src[i] <= 0x7E)) {
                    i++;
                }
            }
            continue; /* i 停在终止字节上，循环的 i++ 跳过它 */
        }
        if (ch == '\r') {
            continue;
        }
        dst[o++] = ch;
    }
    return o;
}

/** esp_log 行首字母 → 级别（1=E … 5=V），认不出的按 INFO。 */
static inline int logfwd_level(const char *s, size_t n)
{
    if (n == 0) {
        return 3;
    }
    switch (s[0]) {
    case 'E': return 1;
    case 'W': return 2;
    case 'I': return 3;
    case 'D': return 4;
    case 'V': return 5;
    default: return 3;
    }
}
