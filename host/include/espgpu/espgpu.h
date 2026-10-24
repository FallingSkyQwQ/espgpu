/*
 * espgpu.h — ESP32-S3-GPU 的 Windows 端公开接口
 *
 * 这一层是"设备接口层"，对应真实 GPU 驱动里的内核态+用户态分界：
 * 上层的 GL 1.2 / GLES 3.0 前端只跟这里打交道，不碰 USB 细节。
 *
 * 线程模型：
 *   - espgpu_open() 会起一个后台接收线程，持续从 bulk IN 读数据并解码帧。
 *   - espgpu_poll() 只做状态检查，不阻塞；帧数据由后台线程准备好。
 *   - espgpu_get_xxx / espgpu_set_xxx 是同步调用，内部有超时。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "espgpu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ESPGPU_BUILD_DLL)
#define ESPGPU_API __declspec(dllexport)
#else
#define ESPGPU_API __declspec(dllimport)
#endif

typedef struct espgpu_device espgpu_device_t;

/** 链路与主机侧统计。 */
typedef struct {
    uint32_t frames_received;
    uint32_t frames_dropped;
    uint32_t fragments_lost;
    uint32_t resyncs;         /**< 因坏帧/半截包而重新对齐的次数 */
    uint32_t crc_errors;
    uint32_t bytes_in;        /**< 设备 -> 主机 */
    uint32_t bytes_out;       /**< 主机 -> 设备 */
    float fps;                /**< 最近 1 秒实测帧率 */
    float in_kbps;            /**< 最近 1 秒上行带宽（KB/s） */
    float out_kbps;           /**< 最近 1 秒下行带宽（KB/s） */
    float decode_ms;          /**< 最近一帧解码耗时 */
    float rtt_ms;             /**< 最近一次控制往返延迟 */
} espgpu_link_stats_t;

/* ── 设备发现 ─────────────────────────────────────────────────────── */

/** 当前枚举到的本设备数量。 */
ESPGPU_API int espgpu_device_count(void);

/** 取第 index 个设备的设备路径（静态缓冲，供界面显示）。 */
ESPGPU_API const char *espgpu_device_path_at(int index);

/** 打开设备。失败返回 NULL 并把原因写进 err。 */
ESPGPU_API espgpu_device_t *espgpu_open(int index, char *err, size_t err_cap);

ESPGPU_API void espgpu_close(espgpu_device_t *dev);

/** 设备是否仍在线（未拔出、未出错）。 */
ESPGPU_API bool espgpu_is_connected(espgpu_device_t *dev);

/** 打开时使用的设备路径。 */
ESPGPU_API const char *espgpu_device_path(espgpu_device_t *dev);

/* ── 控制通道（同步） ─────────────────────────────────────────────── */

ESPGPU_API int espgpu_ping(espgpu_device_t *dev, int timeout_ms);

ESPGPU_API int espgpu_get_info(espgpu_device_t *dev, espgpu_device_info_t *info,
                               int timeout_ms);

ESPGPU_API int espgpu_get_stats(espgpu_device_t *dev, espgpu_stats_t *stats, int timeout_ms);

/** pattern 对应固件里的 ESPGPU_PATTERN_*，仅 M1 测试图案阶段有意义。 */
ESPGPU_API int espgpu_set_mode(espgpu_device_t *dev, const espgpu_mode_t *mode, uint8_t pattern,
                               int timeout_ms);

ESPGPU_API int espgpu_set_clut(espgpu_device_t *dev, const uint16_t *clut, int timeout_ms);

ESPGPU_API int espgpu_reset_stats(espgpu_device_t *dev, int timeout_ms);

/* ── 帧接收 ───────────────────────────────────────────────────────── */

/**
 * 轮询。返回 1 表示自上次调用以来有新帧，0 表示没有，<0 表示设备出错。
 * 不阻塞：真正的接收在后台线程里。
 */
ESPGPU_API int espgpu_poll(espgpu_device_t *dev, int timeout_ms);

/** 最近一帧的 RGBA8888 数据（供 ImGui 直接上传纹理）。 */
ESPGPU_API const uint8_t *espgpu_frame_rgba(espgpu_device_t *dev);

/**
 * 把最近一帧拷贝成 RGBA8888。线程安全，返回 0 表示成功。
 *
 * 优先用这个而不是 espgpu_frame_rgba()：后者返回内部缓冲指针，
 * 后台接收线程可能正在往里写，直接读会撕裂。
 */
ESPGPU_API int espgpu_frame_snapshot(espgpu_device_t *dev, uint8_t *dst, size_t cap,
                                     int *out_w, int *out_h, uint32_t *out_frame_id);

/** 最近一帧的 RGB565 数据（原始格式）。 */
ESPGPU_API const uint16_t *espgpu_frame_rgb565(espgpu_device_t *dev);

ESPGPU_API int espgpu_frame_width(espgpu_device_t *dev);
ESPGPU_API int espgpu_frame_height(espgpu_device_t *dev);
ESPGPU_API uint32_t espgpu_frame_id(espgpu_device_t *dev);

ESPGPU_API const espgpu_link_stats_t *espgpu_link_stats(espgpu_device_t *dev);

/* ── 诊断 ─────────────────────────────────────────────────────────── */

/** 打开日志输出（level: 0=off 1=error 2=warn 3=info 4=debug）。 */
ESPGPU_API void espgpu_set_log_level(int level);

/** 设置日志回调；cb 在接收线程上下文被调用，注意线程安全。 */
typedef void (*espgpu_log_fn)(int level, const char *msg, void *user);
ESPGPU_API void espgpu_set_log_callback(espgpu_log_fn cb, void *user);

#ifdef __cplusplus
}
#endif
