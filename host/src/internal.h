/*
 * internal.h — espgpu.dll 内部结构
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winusb.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "espgpu/espgpu.h"

/* ── 日志 ─────────────────────────────────────────────────────────── */

namespace espgpu {

enum { LOG_OFF = 0, LOG_ERROR = 1, LOG_WARN = 2, LOG_INFO = 3, LOG_DEBUG = 4 };

void log_msg(int level, const char *fmt, ...);
void log_set_level(int level);
void log_set_callback(espgpu_log_fn cb, void *user);

struct PendingCtrl {
    bool done = false;
    uint16_t status = 0;
    std::vector<uint8_t> data;
};

/* ── USB 原始层 ───────────────────────────────────────────────────── */

bool usb_enumerate(std::vector<std::string> &paths);
bool usb_open(espgpu_device *dev, const std::string &path, std::string &err);
void usb_close(espgpu_device *dev);
bool usb_write(espgpu_device *dev, const void *data, size_t len, DWORD timeout_ms);
/** 返回 1=读到数据，0=超时，-1=错误 */
int usb_read(espgpu_device *dev, void *buf, DWORD cap, DWORD timeout_ms, DWORD *got);

/* ── 链路层 ───────────────────────────────────────────────────────── */

void link_start(espgpu_device *dev);
void link_stop(espgpu_device *dev);
int link_send_chunk(espgpu_device *dev, uint8_t type, uint8_t flags, const void *payload,
                    uint32_t len, DWORD timeout_ms);
int link_send_chunk_iov(espgpu_device *dev, uint8_t type, uint8_t flags, const void *p0,
                        uint32_t n0, const void *p1, uint32_t n1, DWORD timeout_ms);
int link_ctrl(espgpu_device *dev, uint16_t op, uint32_t a0, uint32_t a1, const void *data,
              uint16_t len, std::vector<uint8_t> &resp, DWORD timeout_ms);

/* ── 命令缓冲（draw.cpp） ─────────────────────────────────────────── */

/** 单条命令体上限：留出余量，保证任何一条 push 之后都还塞得进一个完整 Chunk */
constexpr size_t kCmdFlushThreshold = 24 * 1024;
constexpr size_t kCmdMaxBody = kCmdFlushThreshold - 64;

void cmd_push(espgpu_device *dev, uint8_t op, uint8_t flags, const void *body, uint32_t size);
int cmd_flush(espgpu_device *dev, int timeout_ms);

/* ── 软件模拟器（emulator.cpp） ──────────────────────────────────── */

constexpr const char *kEmulatorPath = "emulator://espgpu";
bool emulator_enabled();
bool emu_open(espgpu_device *dev, std::string &err);
void emu_close(espgpu_device *dev);
bool emu_write(espgpu_device *dev, const void *data, size_t len, DWORD timeout_ms);
int emu_read(espgpu_device *dev, void *buf, DWORD cap, DWORD timeout_ms, DWORD *got);

/* ── 解码 ─────────────────────────────────────────────────────────── */

int frame_decode(espgpu_device *dev, const uint8_t *enc, size_t enc_len, uint32_t raw_len,
                 int w, int h, uint8_t format, uint8_t codec);

} // namespace espgpu

/* ── 设备对象 ─────────────────────────────────────────────────────── */

struct EmuState;

struct espgpu_device {
    HANDLE file = INVALID_HANDLE_VALUE;
    EmuState *emu = nullptr;    /**< 非空 = 软件模拟器，不走 WinUSB */
    WINUSB_INTERFACE_HANDLE winusb = nullptr;
    UCHAR ep_out = 0;
    UCHAR ep_in = 0;
    std::string path;

    std::thread reader;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> need_keyframe{false}; /**< 解码失败后请求设备重新发关键帧 */

    /* 接收重组缓冲：USB 是字节流，Chunk 可以跨多次读到达 */
    std::vector<uint8_t> rx;
    size_t rx_off = 0;

    /* 帧状态 */
    std::mutex frame_mu;
    std::vector<uint8_t> enc;   /* 当前帧的编码数据（所有分片拼接） */
    std::vector<uint8_t> cur;   /* 解码后的当前帧，设备像素格式 */
    std::vector<uint8_t> prev;  /* 差分参考帧，设备像素格式 */
    std::vector<uint16_t> rgb565;
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    uint8_t format = 0;
    uint8_t codec = 0;
    uint32_t frame_id = 0;
    bool frame_ready = false;
    uint32_t cur_frame_id = 0;
    uint32_t cur_raw_len = 0;
    uint8_t cur_frag_expect = 0;
    uint8_t cur_frag_count = 0;
    bool cur_broken = false;
    uint16_t clut[256] = {};

    /* 控制通道 */
    std::mutex ctrl_mu;
    std::condition_variable ctrl_cv;
    std::map<uint16_t, espgpu::PendingCtrl> pending;
    std::atomic<uint16_t> next_seq{1};

    /* 写串行化：控制请求和命令流共用同一条 OUT 端点 */
    std::mutex write_mu;

    /* M2：命令批量缓冲 */
    std::vector<uint8_t> cmd_buf;

    /* M3+：设备能力、压缩、事件、同步 */
    espgpu_device_info_t info{};
    bool have_info = false;
    std::atomic<bool> compress{false};
    std::vector<uint8_t> lz_buf;
    std::vector<uint32_t> lz_table;
    uint8_t layout_index_type = 1;   /**< 最近一次发出的 VERTEX_LAYOUT 的索引类型 */
    uint16_t layout_stride = 0;

    std::mutex evt_mu;
    std::condition_variable evt_cv;
    std::deque<espgpu_event_t> events;
    uint32_t last_fence = 0;
    bool have_fence = false;

    std::condition_variable frame_cv; /**< 与 frame_mu 配合 */
    std::vector<uint8_t> blit_buf;

    /* 统计 */
    std::mutex stats_mu;
    espgpu_link_stats_t stats{};
    uint64_t win_start_ms = 0;
    uint32_t win_frames = 0;
    uint32_t win_bytes_in = 0;
    uint32_t win_bytes_out = 0;
    uint32_t last_bytes_in = 0;
    uint32_t last_bytes_out = 0;
    uint64_t last_decode_us = 0;
    uint32_t last_polled_frame = 0;
};
