/*
 * api.cpp — 公开接口实现
 *
 * 这一层只做参数校验、状态查询和窗口统计，真正的 I/O 在 link/winusb 里。
 */

#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace espgpu;

namespace {

uint64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void update_window(espgpu_device *dev)
{
    const uint64_t now = now_ms();

    std::lock_guard<std::mutex> lk(dev->stats_mu);
    if (dev->win_start_ms == 0) {
        dev->win_start_ms = now;
        return;
    }
    const uint64_t elapsed = now - dev->win_start_ms;
    if (elapsed < 500) {
        return;
    }
    const float secs = static_cast<float>(elapsed) / 1000.0f;
    dev->stats.fps = static_cast<float>(dev->win_frames) / secs;
    dev->stats.in_kbps = static_cast<float>(dev->win_bytes_in) / secs / 1024.0f;
    dev->stats.out_kbps = static_cast<float>(dev->win_bytes_out) / secs / 1024.0f;

    dev->win_frames = 0;
    dev->win_bytes_in = 0;
    dev->win_bytes_out = 0;
    dev->win_start_ms = now;
}

} // namespace

/* ── 设备发现 ─────────────────────────────────────────────────────── */

extern "C" {

int espgpu_device_count(void)
{
    std::vector<std::string> paths;
    usb_enumerate(paths);
    return static_cast<int>(paths.size());
}

const char *espgpu_device_path_at(int index)
{
    static thread_local std::string s;
    std::vector<std::string> paths;
    usb_enumerate(paths);
    if (index < 0 || index >= static_cast<int>(paths.size())) {
        return nullptr;
    }
    s = paths[static_cast<size_t>(index)];
    return s.c_str();
}

espgpu_device_t *espgpu_open(int index, char *err, size_t err_cap)
{
    auto set_err = [&](const char *msg) {
        if (err && err_cap) {
            snprintf(err, err_cap, "%s", msg);
        }
    };

    std::vector<std::string> paths;
    if (!usb_enumerate(paths)) {
        set_err("no ESP32-S3 GPU found (check USB cable and driver binding)");
        return nullptr;
    }
    if (index < 0 || index >= static_cast<int>(paths.size())) {
        set_err("device index out of range");
        return nullptr;
    }

    auto *dev = new (std::nothrow) espgpu_device();
    if (dev == nullptr) {
        set_err("out of memory");
        return nullptr;
    }

    std::string e;
    if (!usb_open(dev, paths[static_cast<size_t>(index)], e)) {
        set_err(e.c_str());
        delete dev;
        return nullptr;
    }

    /* 默认调色板先填灰阶，设备一上来就会推 CLUT 覆盖它 */
    for (int i = 0; i < 256; i++) {
        const uint16_t v = static_cast<uint16_t>(i);
        dev->clut[i] = static_cast<uint16_t>(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
    }
    dev->win_start_ms = now_ms();

    link_start(dev);

    /* 握一次手，确认对端协议版本与固件活着 */
    espgpu_device_info_t info{};
    const int rc = espgpu_get_info(dev, &info, 1000);
    if (rc != 0) {
        log_msg(LOG_WARN, "handshake failed (rc=%d), device will be opened anyway", rc);
    } else {
        log_msg(LOG_INFO, "device info: fw %u.%u.%u, %ux%u max, psram %uMB, caps 0x%04X",
                info.fw_major, info.fw_minor, info.fw_patch, info.max_width, info.max_height,
                info.psram_mb, info.capabilities);
        dev->info = info;
        dev->have_info = true;
        /* 设备声明能解 LZ 才压缩下行，老固件照旧发原文 */
        dev->compress = (info.capabilities & ESPGPU_CAP_LZ) != 0;
    }

    return dev;
}

void espgpu_close(espgpu_device_t *dev)
{
    if (dev == nullptr) {
        return;
    }
    /* 其他线程可能正阻塞在 link_ctrl / wait_fence / wait_frame / usb_write 里：
     * link_stop 置 running=false、取消管道并唤醒所有条件变量，这里再等它们全部退出，
     * 否则 delete 之后它们醒来就是 use-after-free。 */
    link_stop(dev);
    while (dev->users.load() != 0) {
        usb_abort(dev);
        { std::lock_guard<std::mutex> lk(dev->ctrl_mu); dev->ctrl_cv.notify_all(); }
        { std::lock_guard<std::mutex> lk(dev->frame_mu); dev->frame_cv.notify_all(); }
        { std::lock_guard<std::mutex> lk(dev->evt_mu); dev->evt_cv.notify_all(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    usb_close(dev);
    delete dev;
}

bool espgpu_is_connected(espgpu_device_t *dev)
{
    return dev != nullptr && dev->connected.load();
}

const char *espgpu_device_path(espgpu_device_t *dev)
{
    return dev ? dev->path.c_str() : nullptr;
}

/* ── 控制通道 ─────────────────────────────────────────────────────── */

int espgpu_ping(espgpu_device_t *dev, int timeout_ms)
{
    if (!dev) {
        return -1;
    }
    std::vector<uint8_t> resp;
    return link_ctrl(dev, ESPGPU_CTRL_PING, 0, 0, nullptr, 0, resp, static_cast<DWORD>(timeout_ms));
}

int espgpu_get_info(espgpu_device_t *dev, espgpu_device_info_t *info, int timeout_ms)
{
    if (!dev || !info) {
        return -1;
    }
    std::vector<uint8_t> resp;
    const int st = link_ctrl(dev, ESPGPU_CTRL_GET_DEVICE_INFO, 0, 0, nullptr, 0, resp,
                             static_cast<DWORD>(timeout_ms));
    if (st != ESPGPU_OK || resp.size() < sizeof(*info)) {
        return st != ESPGPU_OK ? st : -1;
    }
    memcpy(info, resp.data(), sizeof(*info));
    dev->info = *info;
    dev->have_info = true;
    return 0;
}

int espgpu_get_stats(espgpu_device_t *dev, espgpu_stats_t *stats, int timeout_ms)
{
    if (!dev || !stats) {
        return -1;
    }
    std::vector<uint8_t> resp;
    const int st = link_ctrl(dev, ESPGPU_CTRL_GET_STATS, 0, 0, nullptr, 0, resp,
                             static_cast<DWORD>(timeout_ms));
    /* 老固件的 stats 只到 seq_gaps：缺的追加字段按 0 处理 */
    constexpr size_t kLegacySize = offsetof(espgpu_stats_t, tex_resident);
    if (st != ESPGPU_OK || resp.size() < kLegacySize) {
        return st != ESPGPU_OK ? st : -1;
    }
    memset(stats, 0, sizeof(*stats));
    memcpy(stats, resp.data(), std::min(resp.size(), sizeof(*stats)));
    return 0;
}

int espgpu_set_mode(espgpu_device_t *dev, const espgpu_mode_t *mode, uint8_t pattern,
                    int timeout_ms)
{
    if (!dev || !mode) {
        return -1;
    }
    std::vector<uint8_t> resp;
    return link_ctrl(dev, ESPGPU_CTRL_SET_MODE, pattern, 0, mode, sizeof(*mode), resp,
                     static_cast<DWORD>(timeout_ms));
}

int espgpu_set_clut(espgpu_device_t *dev, const uint16_t *clut, int timeout_ms)
{
    if (!dev || !clut) {
        return -1;
    }
    /* 先本地生效，避免等待设备回推 CLUT Chunk 期间 INDEX8 显示错色 */
    {
        std::lock_guard<std::mutex> lk(dev->frame_mu);
        memcpy(dev->clut, clut, sizeof(dev->clut));
        dev->clut_dirty = true;
    }
    std::vector<uint8_t> resp;
    return link_ctrl(dev, ESPGPU_CTRL_SET_CLUT, 0, 0, clut, 256 * sizeof(uint16_t), resp,
                     static_cast<DWORD>(timeout_ms));
}

int espgpu_reset_stats(espgpu_device_t *dev, int timeout_ms)
{
    if (!dev) {
        return -1;
    }
    std::vector<uint8_t> resp;
    return link_ctrl(dev, ESPGPU_CTRL_RESET_STATS, 0, 0, nullptr, 0, resp,
                     static_cast<DWORD>(timeout_ms));
}

/* ── 帧 ───────────────────────────────────────────────────────────── */

int espgpu_poll(espgpu_device_t *dev, int timeout_ms)
{
    if (!dev) {
        return -1;
    }
    if (!dev->connected.load()) {
        return -1;
    }

    /* 帧由后台线程准备好，这里只做节流统计 */
    if (timeout_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    }

    update_window(dev);

    /* 解码失败后主动要求一帧关键帧，让差分参考帧重新对齐（不等响应，不卡 UI 线程） */
    link_service_keyframe(dev);

    std::lock_guard<std::mutex> lk(dev->frame_mu);
    if (dev->frame_ready && dev->frame_id != dev->last_polled_frame) {
        dev->last_polled_frame = dev->frame_id;
        dev->frame_ready = false;
        return 1;
    }
    return 0;
}

const uint8_t *espgpu_frame_rgba(espgpu_device_t *dev)
{
    return dev ? dev->rgba.data() : nullptr;
}

int espgpu_frame_snapshot(espgpu_device_t *dev, uint8_t *dst, size_t cap, int *out_w,
                          int *out_h, uint32_t *out_frame_id)
{
    if (!dev || !dst) {
        return -1;
    }

    std::lock_guard<std::mutex> lk(dev->frame_mu);
    const size_t need = dev->rgba.size();
    if (need == 0 || cap < need) {
        return -1;
    }
    memcpy(dst, dev->rgba.data(), need);
    if (out_w) {
        *out_w = dev->width;
    }
    if (out_h) {
        *out_h = dev->height;
    }
    if (out_frame_id) {
        *out_frame_id = dev->frame_id;
    }
    return 0;
}

const uint16_t *espgpu_frame_rgb565(espgpu_device_t *dev)
{
    return dev ? dev->rgb565.data() : nullptr;
}

int espgpu_frame_width(espgpu_device_t *dev)
{
    return dev ? dev->width : 0;
}

int espgpu_frame_height(espgpu_device_t *dev)
{
    return dev ? dev->height : 0;
}

uint32_t espgpu_frame_id(espgpu_device_t *dev)
{
    return dev ? dev->frame_id : 0;
}

const espgpu_link_stats_t *espgpu_link_stats(espgpu_device_t *dev)
{
    if (dev == nullptr) {
        return nullptr;
    }
    /* 接收线程在 stats_mu 下改 dev->stats：返回加锁拷贝出的快照，避免撕裂读 */
    std::lock_guard<std::mutex> lk(dev->stats_mu);
    dev->stats_snapshot = dev->stats;
    return &dev->stats_snapshot;
}

int espgpu_get_link_stats(espgpu_device_t *dev, espgpu_link_stats_t *out)
{
    if (dev == nullptr || out == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lk(dev->stats_mu);
    *out = dev->stats;
    return 0;
}

/* ── 诊断 ─────────────────────────────────────────────────────────── */

void espgpu_set_log_level(int level)
{
    log_set_level(level);
}

void espgpu_set_log_callback(espgpu_log_fn cb, void *user)
{
    log_set_callback(cb, user);
}

} // extern "C"
