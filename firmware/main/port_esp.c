/*
 * port_esp.c — 设备核心在 ESP32-S3 上的平台层
 *
 * 双核光栅：worker 1 是固定在 core1 的任务，PRESENT 时与命令任务（core0）
 * 一起领取 tile。命令模式下 core1 上的测试图案任务是空闲的，不会抢。
 */

#include <stdarg.h>
#include <stdio.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "port.h"

static const char *TAG = "core";

void *gp_alloc_fast(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    }
    return p;
}

void *gp_alloc_big(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    }
    return p;
}

void gp_free(void *p)
{
    heap_caps_free(p);
}

int gp_is_fast(const void *p)
{
    return esp_ptr_internal(p) ? 1 : 0;
}

uint64_t gp_time_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

void gp_log(int level, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    switch (level) {
    case GP_LOG_ERROR: ESP_LOGE(TAG, "%s", buf); break;
    case GP_LOG_WARN: ESP_LOGW(TAG, "%s", buf); break;
    case GP_LOG_INFO: ESP_LOGI(TAG, "%s", buf); break;
    default: ESP_LOGD(TAG, "%s", buf); break;
    }
}

/* 分段计时（统计里的 t_vtx_us / t_setup_us / t_tile_us …）：读 CCOUNT 约 8 个周期 */
IRAM_ATTR uint32_t gp_cycles(void)
{
    return (uint32_t)esp_cpu_get_cycle_count();
}

uint32_t gp_cycles_per_us(void)
{
    const uint32_t r = esp_rom_get_cpu_ticks_per_us(); /* 跟随 SET_CLOCK / esp_pm */
    return r ? r : 240;
}

int gp_atomic_fetch_add(volatile int *p, int v)
{
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

/* ── 第二个光栅 worker ────────────────────────────────────────────── */

#if CONFIG_ESPGPU_RASTER_DUAL_CORE

static TaskHandle_t s_worker;
static SemaphoreHandle_t s_done;
static gp_job_fn s_fn;
static void *s_ctx;

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_fn) {
            s_fn(s_ctx, 1);
        }
        xSemaphoreGive(s_done);
    }
}

static void ensure_worker(void)
{
    if (s_worker) {
        return;
    }
    s_done = xSemaphoreCreateBinary();
    if (s_done == NULL) {
        return;
    }
    /* 优先级高于 core1 上的呈现任务（6），保证 PRESENT 时立刻开工 */
    /* 6 KB：通用路径的跨度缓冲 + gp_vm_run_span（整段 8 像素一起跑 VM）约多 1.2 KB */
    if (xTaskCreatePinnedToCore(worker_task, "gpu_raster1", 6144, NULL, 8, &s_worker, 1) !=
        pdPASS) {
        s_worker = NULL;
    }
}

int gp_worker_count(void)
{
    ensure_worker();
    return s_worker ? 2 : 1;
}

void gp_parallel(gp_job_fn fn, void *ctx)
{
    if (s_worker == NULL) {
        fn(ctx, 0);
        return;
    }
    s_fn = fn;
    s_ctx = ctx;
    xTaskNotifyGive(s_worker);
    fn(ctx, 0);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

#else

int gp_worker_count(void)
{
    return 1;
}

void gp_parallel(gp_job_fn fn, void *ctx)
{
    fn(ctx, 0);
}

#endif
