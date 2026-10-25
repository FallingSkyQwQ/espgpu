/*
 * gl_device.cpp — 设备连接与 SwapBuffers（M4 的 "espgpu video backend"）
 *
 * SwapBuffers：
 *   1. 把待发批次发掉，补 PRESENT
 *   2. 等设备回传新的一帧（有上限的等待：背压 + 帧同步，PLAN R5）
 *   3. 按 ESPGPU_FPS 节流（timeBeginPeriod(1)，见 docs/perf-notes.md 第六步）
 *   4. 把最新一帧 StretchDIBits 到应用自己的窗口
 *
 * 环境变量：
 *   ESPGPU_RES        设备分辨率，默认 320x200
 *   ESPGPU_FPS        目标帧率，默认 24
 *   ESPGPU_GL_LZ      下行压缩，默认 1
 *   ESPGPU_GL_WAIT_MS 每帧等设备回帧的上限，默认 2 个帧周期
 */

#include "gl_context.h"

#include <mmsystem.h>

#include <cstdio>
#include <cstdlib>

namespace espgl {

namespace {

bool g_warned_disconnect = false;

void parse_res(int &w, int &h)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA("ESPGPU_RES", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return;
    }
    int a = 0, b = 0;
    if (sscanf(buf, "%dx%d", &a, &b) == 2 && a >= 32 && b >= 32 && a <= 800 && b <= 600) {
        w = a;
        h = b;
    }
}

} // namespace

bool device_open()
{
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    if (g_sh.dev_tried) {
        return g_sh.dev != nullptr;
    }
    g_sh.dev_tried = true;

    parse_res(g_sh.dev_w, g_sh.dev_h);
    g_sh.fps = env_int("ESPGPU_FPS", 24);
    if (g_sh.fps < 1) g_sh.fps = 1;
    if (g_sh.fps > 60) g_sh.fps = 60;

    if (espgpu_device_count() <= 0) {
        log_line("no ESP32-S3 GPU found");
        return false;
    }
    char err[256] = {0};
    espgpu_device_t *dev = espgpu_open(0, err, sizeof(err));
    if (dev == nullptr) {
        log_line("espgpu_open failed: %s", err);
        return false;
    }

    espgpu_mode_t m{};
    m.width = static_cast<uint16_t>(g_sh.dev_w);
    m.height = static_cast<uint16_t>(g_sh.dev_h);
    m.format = ESPGPU_FMT_RGB565;
    m.codec = ESPGPU_CODEC_DELTA_RLE;
    m.max_fps = static_cast<uint8_t>(g_sh.fps);
    m.vsync = 1;
    const int st = espgpu_set_mode(dev, &m, 0, 1500);
    if (st != 0) {
        log_line("set_mode %dx%d failed (status %d), continuing", g_sh.dev_w, g_sh.dev_h, st);
    }

    if (const espgpu_device_info_t *info = espgpu_cached_info(dev)) {
        if (info->max_texture_dim >= 16) {
            g_sh.max_tex_dim = info->max_texture_dim;
        }
        const uint64_t cache = info->texture_cache_size ? info->texture_cache_size : 4u * 1024 * 1024;
        g_sh.tex_budget = cache * 85 / 100;
        log_line("device: fw %u.%u.%u, caps 0x%04X, tex cache %u KB, max tex %u", info->fw_major, info->fw_minor,
                 info->fw_patch, info->capabilities, info->texture_cache_size / 1024, info->max_texture_dim);
    }
    espgpu_set_compression(dev, env_int("ESPGPU_GL_LZ", 1) != 0);

    timeBeginPeriod(1);
    QueryPerformanceFrequency(&g_sh.qpf);
    QueryPerformanceCounter(&g_sh.next_frame_qpc);
    g_sh.last_frame_id = espgpu_frame_id(dev);
    g_sh.dev = dev;
    log_line("espgpu GL front-end: device %dx%d @ %d fps%s", g_sh.dev_w, g_sh.dev_h, g_sh.fps,
             espgpu_is_emulator(dev) ? " (emulator)" : "");
    return true;
}

void device_poll_events()
{
    espgpu_device_t *dev = g_sh.dev;
    if (dev == nullptr) {
        return;
    }
    espgpu_event_t ev;
    int guard = 0;
    while (guard++ < 1024 && espgpu_pop_event(dev, &ev) == 1) {
        switch (ev.kind) {
        case ESPGPU_EVT_TEXTURE_EVICTED: tex_on_evicted(static_cast<uint16_t>(ev.arg)); break;
        case ESPGPU_EVT_BUFFER_EVICTED: list_on_evicted(ev.arg); break;
        case ESPGPU_EVT_ERROR: log_line("device error event code=%u value=%u arg=%u", ev.code, ev.value, ev.arg); break;
        default: break;
        }
    }
}

void device_present(Context *c)
{
    espgpu_device_t *dev = g_sh.dev;
    if (dev == nullptr || c == nullptr) {
        return;
    }
    flush_pending(c);

    if (!espgpu_is_connected(dev)) {
        if (!g_warned_disconnect) {
            log_line("device disconnected; frames are dropped");
            g_warned_disconnect = true;
        }
        c->pending.clear();
        return;
    }

    espgpu_cmd_present(dev, 1000);

    /* 帧周期 */
    const LONGLONG period = g_sh.qpf.QuadPart / g_sh.fps;
    const int wait_ms = env_int("ESPGPU_GL_WAIT_MS", static_cast<int>(2000 / g_sh.fps));
    if (espgpu_wait_frame(dev, g_sh.last_frame_id, wait_ms) == 1) {
        g_sh.last_frame_id = espgpu_frame_id(dev);
    }

    /* 节流：按截止时间睡，落后太多就重新对齐，避免追帧 */
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_sh.next_frame_qpc.QuadPart += period;
    if (g_sh.next_frame_qpc.QuadPart < now.QuadPart - period) {
        g_sh.next_frame_qpc = now;
    } else if (g_sh.next_frame_qpc.QuadPart > now.QuadPart) {
        const LONGLONG ms = (g_sh.next_frame_qpc.QuadPart - now.QuadPart) * 1000 / g_sh.qpf.QuadPart;
        if (ms > 0 && c->swap_interval != 0) {
            Sleep(static_cast<DWORD>(ms));
        }
    }

    if (c->hwnd) {
        RECT rc;
        if (GetClientRect(c->hwnd, &rc)) {
            c->win_w = std::max(1L, rc.right - rc.left);
            c->win_h = std::max(1L, rc.bottom - rc.top);
        }
    }
    HDC hdc = c->hdc ? c->hdc : (c->hwnd ? GetDC(c->hwnd) : nullptr);
    if (hdc) {
        espgpu_blit_to_hdc(dev, hdc, c->win_w, c->win_h);
        if (!c->hdc && c->hwnd) {
            ReleaseDC(c->hwnd, hdc);
        }
    }

    g_sh.frame++;
    device_poll_events();
}

} // namespace espgl
