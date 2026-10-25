/*
 * core.h — 设备核心（可移植 C，不依赖 ESP-IDF）
 *
 * 固件和主机端软件模拟器共用这一份实现：
 *   命令流执行 → T&L（顶点变换 / 光照 / 雾 / 裁剪 / 剔除）→ tile 分箱
 *   → PRESENT 时逐 tile 在内部 SRAM 光栅化（透视校正 / 双线性 / alpha / Z / 混合 / 片元 VM）
 *   → 回调把整帧 RGB565 交给平台层去编码和发送。
 *
 * 线程模型：所有 gpu_core_* 调用必须来自同一个线程（命令任务）。
 * 光栅阶段可以通过 port.h 的 gp_parallel() 分到多个核上，核心内部负责切分。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espgpu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gpu_core gpu_core_t;

typedef struct {
    /** 设备要发给主机的事件（纹理换出、fence 等）。 */
    void (*send_event)(void *user, const espgpu_event_t *ev);
    /**
     * 一帧渲染完成（PRESENT）。pixels 为 w*h 的 RGB565，行主序、第 0 行在上。
     * 指针只在回调期间有效。
     */
    void (*present)(void *user, const uint16_t *pixels, uint16_t w, uint16_t h);
    void *user;
} gpu_core_cb_t;

typedef struct {
    uint32_t arena_bytes;        /**< 分箱内存（三角形记录 + 状态快照），建议 ≥ 1 MB */
    uint32_t texture_budget;     /**< 纹理缓存上限（字节） */
    uint32_t buffer_budget;      /**< 设备端顶点 / 索引缓冲上限 */
    uint16_t max_texture_dim;
} gpu_core_config_t;

gpu_core_t *gpu_core_create(const gpu_core_config_t *cfg, const gpu_core_cb_t *cb);
void gpu_core_destroy(gpu_core_t *c);

/** 改屏幕分辨率（丢弃当前帧内容与未完成的分箱）。 */
bool gpu_core_set_size(gpu_core_t *c, uint16_t w, uint16_t h);

/** 执行一个 CMD chunk 的载荷（已解压）。 */
void gpu_core_exec(gpu_core_t *c, const uint8_t *cmds, uint32_t len);

/** 处理一个 DATA chunk 的载荷（已解压）。 */
void gpu_core_data(gpu_core_t *c, const uint8_t *payload, uint32_t len);

void gpu_core_set_debug_view(gpu_core_t *c, uint8_t view);

/** 把核心相关的统计字段写进 out（不碰平台字段）。 */
void gpu_core_fill_stats(gpu_core_t *c, espgpu_stats_t *out);

/** 每次 PRESENT 后会被清零的逐帧计数可以在这里取。 */
uint32_t gpu_core_frame_counter(gpu_core_t *c);

#ifdef __cplusplus
}
#endif
