/*
 * port_host.c — 主机端（软件模拟器 / 单元测试）的平台层
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "port.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

void *gp_alloc_fast(size_t n)
{
    return malloc(n);
}

void *gp_alloc_big(size_t n)
{
    return malloc(n);
}

void gp_free(void *p)
{
    free(p);
}

int gp_is_fast(const void *p)
{
    (void)p;
    return 1; /* 主机内存没有快慢之分：直接光栅，省掉 tile 拷贝 */
}

uint64_t gp_time_us(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    /* 先除后乘：QuadPart * 1e6 在 10 MHz 的 QPC 下约 21 天就溢出 */
    const uint64_t q = (uint64_t)now.QuadPart, f = (uint64_t)freq.QuadPart;
    return (q / f) * 1000000ull + (q % f) * 1000000ull / f;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

static void (*s_log_hook)(int level, const char *msg);

void gp_host_set_log_hook(void (*fn)(int level, const char *msg))
{
    s_log_hook = fn;
}

void gp_log(int level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_log_hook) {
        s_log_hook(level, buf);
    } else if (level <= GP_LOG_WARN) {
        fprintf(stderr, "[gpu-core] %s\n", buf);
    }
}

void gp_parallel(gp_job_fn fn, void *ctx)
{
    fn(ctx, 0);
}

int gp_worker_count(void)
{
    return 1;
}

int gp_atomic_fetch_add(volatile int *p, int v)
{
    const int old = *p;
    *p = old + v;
    return old;
}
