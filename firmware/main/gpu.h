#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "espgpu_proto.h"
#include "framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 设备全局状态。单实例，由 gpu.c 持有。 */
typedef struct {
    espgpu_mode_t mode;
    espgpu_fb_t fb;
    uint16_t clut[256];

    uint8_t pattern;      /* ESPGPU_PATTERN_*，M1 的测试图案选择 */
    bool streaming;       /* 是否在持续出帧 */
    bool force_keyframe;  /* 模式变更后强制出一帧非差分，让主机重建参考帧 */

    uint32_t frame_id;
    uint32_t present_period_us;

    uint8_t *enc_buf;
    size_t enc_cap;

    espgpu_stats_t stats;

    SemaphoreHandle_t lock; /* 保护 mode / fb 重建 */
} espgpu_gpu_t;

extern espgpu_gpu_t g_gpu;

esp_err_t espgpu_gpu_init(void);

/** 修改显示模式。尺寸或格式变化时重建帧缓冲。 */
esp_err_t espgpu_gpu_set_mode(const espgpu_mode_t *mode, uint8_t pattern);

/** 覆盖调色板。 */
void espgpu_gpu_set_clut(const uint16_t *clut);

/** 呈现任务：按目标帧率生成/编码/发送。 */
void espgpu_gpu_present_task(void *arg);

/** 命令处理任务：读 Chunk、分发、回响应。 */
void espgpu_gpu_command_task(void *arg);

/** 采集瞬时统计（温度、内存等）。 */
void espgpu_gpu_sample_stats(void);

#ifdef __cplusplus
}
#endif
