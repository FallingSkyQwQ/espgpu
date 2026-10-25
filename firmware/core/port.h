/*
 * port.h — 设备核心的平台抽象
 *
 * 固件：port_esp.c（heap_caps / esp_timer / ESP_LOG / 双核 worker）
 * 主机：port_host.c（malloc / steady clock / stderr / 单线程）
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 优先内部 SRAM（tile 缓冲、后变换缓存），失败可退回慢内存。 */
void *gp_alloc_fast(size_t n);
/** 大块（PSRAM）。 */
void *gp_alloc_big(size_t n);
void gp_free(void *p);
/** 指针是否在快速内存里（决定屏幕目标能否直接光栅、免 tile 拷贝）。 */
int gp_is_fast(const void *p);

uint64_t gp_time_us(void);

/**
 * 分段计时用的廉价单调计数器，只用来做差（32 位回绕无妨，单段远小于回绕周期）。
 * ESP：CPU 周期（CCOUNT，每核各自计数，worker 只在自己的核上做差）；主机：TSC / 系统计数器。
 * 每帧每个阶段累加几千次，读一次必须只有几个周期，不能走 esp_timer / 系统调用。
 */
uint32_t gp_cycles(void);
/** gp_cycles() 每微秒的计数（ESP：CPU MHz）。可以慢，每帧只调一次。 */
uint32_t gp_cycles_per_us(void);

enum { GP_LOG_ERROR = 1, GP_LOG_WARN = 2, GP_LOG_INFO = 3, GP_LOG_DEBUG = 4 };
void gp_log(int level, const char *fmt, ...);

/**
 * 并行执行 fn(ctx, worker)，worker ∈ [0, gp_worker_count())，全部返回后才返回。
 * worker 0 在调用者线程上执行。
 */
typedef void (*gp_job_fn)(void *ctx, int worker);
void gp_parallel(gp_job_fn fn, void *ctx);
int gp_worker_count(void);

/** 原子取号（tile 分发用）。 */
int gp_atomic_fetch_add(volatile int *p, int v);

/** 热路径属性：固件上放 IRAM。 */
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define GP_HOT IRAM_ATTR
#else
#define GP_HOT
#endif

#define GP_MAX_WORKERS 2

#ifdef __cplusplus
}
#endif
