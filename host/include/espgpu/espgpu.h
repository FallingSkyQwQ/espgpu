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
#include "espgpu_vm.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ESPGPU_STATIC)
#define ESPGPU_API
#elif defined(_WIN32)
#if defined(ESPGPU_BUILD_DLL)
#define ESPGPU_API __declspec(dllexport)
#else
#define ESPGPU_API __declspec(dllimport)
#endif
#else
#define ESPGPU_API __attribute__((visibility("default")))
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

/* ── M2：绘制命令 ─────────────────────────────────────────────────── */

/**
 * 命令是批量提交的：下面这些函数把命令追加进主机侧缓冲，
 * 缓冲满或调用 espgpu_cmd_present() 时才真正发出去。
 * 这样一帧通常只占一个 USB Chunk。
 */

ESPGPU_API int espgpu_cmd_clear(espgpu_device_t *dev, uint16_t color);

ESPGPU_API int espgpu_cmd_set_pipeline(espgpu_device_t *dev, uint8_t blend, uint8_t filter,
                                       uint8_t wrap, uint8_t alpha_test, uint8_t alpha_ref);

/**
 * 上传纹理。format 支持 ESPGPU_FMT_RGB565 与 ESPGPU_FMT_RGBA4444。
 * 超过单个 Chunk 会自动分片。
 */
ESPGPU_API int espgpu_cmd_upload_texture(espgpu_device_t *dev, uint32_t id, uint16_t width,
                                         uint16_t height, uint8_t format, const void *pixels);

/** 绑定纹理到纹理单元 0。id 传 0 表示解绑。 */
ESPGPU_API int espgpu_cmd_bind_texture(espgpu_device_t *dev, uint32_t id, uint8_t filter,
                                       uint8_t wrap);

/** 提交四边形。顶点为屏幕坐标（1/16 像素）+ 纹理坐标（1/16 texel）。 */
ESPGPU_API int espgpu_cmd_draw_quads(espgpu_device_t *dev, const espgpu_vertex2d_t *verts,
                                     uint32_t quad_count);

ESPGPU_API int espgpu_cmd_scissor(espgpu_device_t *dev, int16_t x, int16_t y, int16_t w,
                                  int16_t h);

/** 结束一帧：补上 PRESENT，把整批命令发出去。 */
ESPGPU_API int espgpu_cmd_present(espgpu_device_t *dev, int timeout_ms);

/** 丢弃尚未发送的命令。 */
ESPGPU_API void espgpu_cmd_discard(espgpu_device_t *dev);


/* ── M3-M6：通用 3D 命令（GL 1.2 / GLES 3.0 前端共用） ─────────────── */

/**
 * 追加一条任意命令。body 最大约 23 KB；更大的数据请用下面的专用接口（会自动分片）。
 */
ESPGPU_API int espgpu_cmd_raw(espgpu_device_t *dev, uint8_t op, uint8_t flags, const void *body,
                              uint32_t size);

/** 完整管线状态。fog / ext 可为 NULL（不发送对应的追加段）。 */
ESPGPU_API int espgpu_cmd_pipeline(espgpu_device_t *dev, const espgpu_pipeline_t *p,
                                   const espgpu_pipeline_fog_t *fog,
                                   const espgpu_pipeline_ext_t *ext);

/** 设置矩阵。m 为行主序 float（clip = M * v）；m == NULL 表示单位阵。 */
ESPGPU_API int espgpu_cmd_matrix(espgpu_device_t *dev, uint8_t slot, const float m[16]);

ESPGPU_API int espgpu_cmd_vertex_layout(espgpu_device_t *dev, const espgpu_vertex_layout_t *l);

/**
 * 3D 绘制，顶点格式由最近一次 espgpu_cmd_vertex_layout 决定。
 * indices 可为 NULL；index_type 1 = u16，2 = u32。
 * 超过单条命令容量时自动拆批（strip / fan / loop 会被展开成列表）。
 */
ESPGPU_API int espgpu_cmd_draw(espgpu_device_t *dev, uint8_t prim, const void *verts,
                               uint32_t vertex_count, uint32_t stride, const void *indices,
                               uint32_t index_count, uint8_t index_type);

/** 从设备端缓冲绘制（显示列表 / 静态几何）。index_count = 0 为非索引。 */
ESPGPU_API int espgpu_cmd_draw_buffer(espgpu_device_t *dev, uint8_t prim, uint32_t buffer_id,
                                      uint32_t base_vertex, uint32_t vertex_count,
                                      uint32_t index_offset, uint32_t index_count);

ESPGPU_API int espgpu_cmd_bind_texture_unit(espgpu_device_t *dev, uint8_t unit, uint32_t id,
                                            uint8_t filter, uint8_t wrap);

/**
 * 上传纹理，format = RGB565 / RGBA4444（2 B/px）或 INDEX8 / R8（1 B/px）。
 * flags = ESPGPU_TEXF_*。
 */
ESPGPU_API int espgpu_cmd_upload_texture_ex(espgpu_device_t *dev, uint32_t id, uint16_t width,
                                            uint16_t height, uint8_t format, const void *pixels,
                                            uint8_t flags);

ESPGPU_API int espgpu_cmd_tex_palette(espgpu_device_t *dev, uint32_t id, uint16_t first,
                                      uint16_t count, const uint16_t *rgb565,
                                      const uint8_t *alpha);

ESPGPU_API int espgpu_cmd_delete_texture(espgpu_device_t *dev, uint32_t id);

/** flags: bit0 颜色 bit1 深度。depth 为 16 位深度值（0xFFFF = 最远）。 */
ESPGPU_API int espgpu_cmd_clear_ex(espgpu_device_t *dev, uint8_t flags, uint16_t color,
                                   uint16_t depth);

/** 视口，左上角原点。 */
ESPGPU_API int espgpu_cmd_viewport(espgpu_device_t *dev, int16_t x, int16_t y, int16_t w,
                                   int16_t h);

ESPGPU_API int espgpu_cmd_depth_range(espgpu_device_t *dev, float znear, float zfar);

ESPGPU_API int espgpu_cmd_color(espgpu_device_t *dev, uint8_t r, uint8_t g, uint8_t b,
                                uint8_t a);

ESPGPU_API int espgpu_cmd_lighting(espgpu_device_t *dev, bool enable, const float ambient[3],
                                   const espgpu_light_t *lights, uint32_t count);

ESPGPU_API int espgpu_cmd_load_program(espgpu_device_t *dev, uint8_t program_id,
                                       uint8_t varying_count, const espgpu_vm_insn_t *code,
                                       uint32_t insn_count);

/** vec4s 为 count × 4 个 float。 */
ESPGPU_API int espgpu_cmd_set_uniforms(espgpu_device_t *dev, uint8_t first, uint8_t count,
                                       const float *vec4s);

ESPGPU_API int espgpu_cmd_fbo(espgpu_device_t *dev, const espgpu_fbo_t *f);

ESPGPU_API int espgpu_cmd_fence(espgpu_device_t *dev, uint32_t value);

/** 把已攒的命令立刻发出（不加 PRESENT）。 */
ESPGPU_API int espgpu_cmd_flush(espgpu_device_t *dev, int timeout_ms);

/** 阻塞等待设备回 FENCE 事件且 value 达到（>=）给定值。0 = 成功。 */
ESPGPU_API int espgpu_wait_fence(espgpu_device_t *dev, uint32_t value, int timeout_ms);

/** 阻塞等待帧号 > after_frame_id 的帧解码完成。返回 1 = 有新帧，0 = 超时，<0 = 出错。 */
ESPGPU_API int espgpu_wait_frame(espgpu_device_t *dev, uint32_t after_frame_id, int timeout_ms);

/** 上传 / 删除设备端缓冲（DATA chunk，自动分片）。 */
ESPGPU_API int espgpu_buffer_upload(espgpu_device_t *dev, uint32_t id, const void *data,
                                    uint32_t len);
ESPGPU_API int espgpu_buffer_delete(espgpu_device_t *dev, uint32_t id);

/** 下行 CMD/DATA chunk 是否尝试 LZ 压缩（设备上报 ESPGPU_CAP_LZ 时默认开）。 */
ESPGPU_API void espgpu_set_compression(espgpu_device_t *dev, bool enable);

/** 打开时读到的设备信息（握手失败则全零）。 */
ESPGPU_API const espgpu_device_info_t *espgpu_cached_info(espgpu_device_t *dev);

/**
 * 拷贝最近一帧为 BGRA8888（GDI StretchDIBits 直接可用的字节序）。
 * pitch 为目标行字节数（0 = w*4）。返回 0 成功。
 */
ESPGPU_API int espgpu_frame_snapshot_bgra(espgpu_device_t *dev, uint8_t *dst, size_t cap,
                                          size_t pitch, int *out_w, int *out_h,
                                          uint32_t *out_frame_id);

/* ── 事件 ─────────────────────────────────────────────────────────── */

/**
 * 取出一个设备事件（纹理换出、fence、OTA 进度……）。返回 1 = 取到，0 = 队列空。
 * 队列最多缓存 1024 个，溢出丢最旧的。
 */
ESPGPU_API int espgpu_pop_event(espgpu_device_t *dev, espgpu_event_t *ev);

/* ── M5：面板功能 ─────────────────────────────────────────────────── */

/** CPU 频率：80 / 160 / 240 MHz。 */
ESPGPU_API int espgpu_set_clock(espgpu_device_t *dev, int mhz, int timeout_ms);

ESPGPU_API int espgpu_set_debug_view(espgpu_device_t *dev, int view, int timeout_ms);

/** 命令模式下 PRESENT 的帧编码（ESPGPU_CODEC_*）。 */
ESPGPU_API int espgpu_set_codec(espgpu_device_t *dev, int codec, int timeout_ms);

/** 订阅设备日志（level 同 espgpu_set_log_level，0 = 取消）。日志经日志回调输出。 */
ESPGPU_API int espgpu_log_subscribe(espgpu_device_t *dev, int level, int timeout_ms);

ESPGPU_API int espgpu_soft_reset(espgpu_device_t *dev, int timeout_ms);

/** OTA 进度回调：done / total 字节。返回 false 取消。 */
typedef bool (*espgpu_progress_fn)(uint32_t done, uint32_t total, void *user);

/**
 * 刷写固件（阻塞）。image 是 idf.py build 产出的 espgpu.bin。
 * 成功后设备切换启动分区，需调用 espgpu_soft_reset() 重启生效。返回 0 成功。
 */
ESPGPU_API int espgpu_ota_flash(espgpu_device_t *dev, const void *image, size_t len,
                                const char *version, espgpu_progress_fn cb, void *user);

/**
 * 把最近一帧缩放画到 Windows HDC（void* 以免公开头依赖 windows.h）。
 * 保持宽高比、黑边居中，dst_w/dst_h 为客户区尺寸。GL / EGL 前端的 SwapBuffers 用它。
 * 非 Windows 平台返回 -1。
 */
ESPGPU_API int espgpu_blit_to_hdc(espgpu_device_t *dev, void *hdc, int dst_w, int dst_h);

/** 当前连接是否是软件模拟器（环境变量 ESPGPU_EMULATOR=1 时打开的是模拟器）。 */
ESPGPU_API bool espgpu_is_emulator(espgpu_device_t *dev);

/* ── 诊断 ─────────────────────────────────────────────────────────── */

/** 打开日志输出（level: 0=off 1=error 2=warn 3=info 4=debug）。 */
ESPGPU_API void espgpu_set_log_level(int level);

/** 设置日志回调；cb 在接收线程上下文被调用，注意线程安全。 */
typedef void (*espgpu_log_fn)(int level, const char *msg, void *user);
ESPGPU_API void espgpu_set_log_callback(espgpu_log_fn cb, void *user);

#ifdef __cplusplus
}
#endif
