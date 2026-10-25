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
#include <cstring>
#include <vector>

namespace espgl {

namespace {

/**
 * ESPGPU_GL_DUMP=<path>：到指定帧（ESPGPU_GL_DUMP_FRAME，默认 90）时把设备回传的
 * 帧存成 24 位 BMP。窗口被遮住 / 用 CopyFromScreen 抓不到 GL 画面时，用它确认
 * 设备侧光栅器的真实输出（截图来自设备，不是主机窗口）。
 */
void maybe_dump_frame(espgpu_device_t *dev)
{
    static const char *s_path = nullptr;
    static int s_frame = -1;
    static bool s_init = false;
    if (!s_init) {
        s_init = true;
        s_path = getenv("ESPGPU_GL_DUMP");
        const char *n = getenv("ESPGPU_GL_DUMP_FRAME");
        s_frame = n ? atoi(n) : 90;
    }
    if (!s_path || !*s_path || static_cast<int>(g_sh.frame) != s_frame) {
        return;
    }
    const int w = espgpu_frame_width(dev);
    const int h = espgpu_frame_height(dev);
    if (w <= 0 || h <= 0) {
        return;
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    if (espgpu_frame_snapshot(dev, rgba.data(), rgba.size(), nullptr, nullptr, nullptr) != 0) {
        return;
    }
    const int row_bytes = (w * 3 + 3) & ~3;
    const int data_size = row_bytes * h;
    const int file_size = 54 + data_size;
    FILE *f = nullptr;
    if (fopen_s(&f, s_path, "wb") != 0 || f == nullptr) {
        return;
    }
    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    memcpy(&hdr[2], &file_size, 4);
    const int off = 54;
    memcpy(&hdr[10], &off, 4);
    const int dib = 40;
    memcpy(&hdr[14], &dib, 4);
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &h, 4);
    const uint16_t planes = 1, bpp = 24;
    memcpy(&hdr[26], &planes, 2);
    memcpy(&hdr[28], &bpp, 2);
    memcpy(&hdr[34], &data_size, 4);
    fwrite(hdr, 1, sizeof(hdr), f);
    std::vector<uint8_t> row(row_bytes, 0);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            const uint8_t *p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
            row[static_cast<size_t>(x) * 3 + 0] = p[2]; /* B */
            row[static_cast<size_t>(x) * 3 + 1] = p[1]; /* G */
            row[static_cast<size_t>(x) * 3 + 2] = p[0]; /* R */
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    log_line("frame %u dumped to %s (%dx%d)", static_cast<unsigned>(g_sh.frame), s_path, w, h);
}

bool g_warned_disconnect = false;
bool g_dev_lost = false; /* 掉线后旧设备已关、新设备还没打开 */
LONGLONG g_next_reopen_qpc = 0;
int g_wait_ms = -1;

/** 打开第 0 个设备并设置模式；失败返回 nullptr */
espgpu_device_t *open_and_setup()
{
    if (espgpu_device_count() <= 0) {
        return nullptr;
    }
    char err[256] = {0};
    espgpu_device_t *dev = espgpu_open(0, err, sizeof(err));
    if (dev == nullptr) {
        log_line("espgpu_open failed: %s", err);
        return nullptr;
    }
    espgpu_mode_t m{};
    m.width = static_cast<uint16_t>(g_sh.dev_w);
    m.height = static_cast<uint16_t>(g_sh.dev_h);
    m.format = ESPGPU_FMT_RGB565;
    /* AUTO：静止无损、动起来改发有损块编码（USB FS 装不下整屏变化的无损帧，issue #2） */
    m.codec = espgpu_preferred_codec(dev);
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
    return dev;
}

/**
 * 设备掉线后（拔线 / 固件重启）espgpu.dll 不会自己重连：隔一秒试着重新打开。
 * 新设备上什么都没有——纹理、列表缓冲、设备状态缓存全部作废，用到时重传。
 */
void try_reopen()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart < g_next_reopen_qpc) {
        return;
    }
    g_next_reopen_qpc = now.QuadPart + g_sh.qpf.QuadPart;
    if (g_sh.dev) {
        /* 旧句柄先关（WinUSB 独占；它的接收线程也该退出了） */
        espgpu_close(g_sh.dev);
        g_sh.dev = nullptr;
        g_dev_lost = true;
    }
    espgpu_device_t *dev = open_and_setup();
    if (dev == nullptr) {
        return;
    }
    g_sh.dev = dev;
    g_dev_lost = false;
    for (auto &kv : g_sh.textures) {
        Texture *t = kv.second.get();
        if (t->resident && t->device_only) {
            t->has_image = false; /* 只存在设备上的内容丢了 */
        }
        t->resident = false;
        t->upload = true;
    }
    g_sh.default_tex.resident = false;
    g_sh.default_tex.upload = true;
    g_sh.tex_used = 0;
    for (auto &kv : g_sh.lists) {
        kv.second->buffer_valid = false;
    }
    g_sh.list_buffers = 0;
    g_sh.list_buffer_bytes = 0;
    for (Context *c : g_sh.contexts) {
        c->dc = DevCache();
        c->key_dirty = true;
    }
    g_sh.dc_owner = nullptr;
    g_sh.bind_epoch++;
    g_sh.last_frame_id = espgpu_frame_id(dev);
    g_sh.inflight = false;
    g_warned_disconnect = false;
    log_line("device reconnected");
}

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
    g_wait_ms = env_int("ESPGPU_GL_WAIT_MS", 2000 / g_sh.fps);

    espgpu_device_t *dev = open_and_setup();
    if (dev == nullptr) {
        log_line("no ESP32-S3 GPU found");
        return false;
    }

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
    if (c == nullptr) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    if (g_sh.dev == nullptr) {
        if (g_dev_lost) {
            c->pending.clear();
            c->parked.clear();
            try_reopen();
            g_sh.frame++;
        }
        return;
    }
    espgpu_device_t *dev = g_sh.dev;
    if (!espgpu_is_connected(dev)) {
        if (!g_warned_disconnect) {
            log_line("device disconnected; frames are dropped until it comes back");
            g_warned_disconnect = true;
        }
        g_sh.inflight = false;
        try_reopen();
        g_sh.frame++;
        return;
    }

    /* 背压：设备比目标帧率慢时（Doom 在 320x200 上可能只有几 fps），
     * 只等一个帧周期会让主机每秒塞进几十帧，命令在设备端越排越多，
     * 控制请求和画面延迟都跟着崩。所以每帧都要等，但等的是「上一帧在设备上已经产出」
     * （PRESENT 后面跟一个 FENCE：光栅 + 编码完、交给发送任务时设备回 FENCE 事件），
     * 而且是先把这一帧的命令发出去再等：
     *   - 设备光栅上一帧的同时，这一帧的命令已经在路上（以前要等上一帧整个回传到主机
     *     才开始发，下行命令、光栅、上行回传三段完全串行，issue #5）；
     *   - 上行回传和下一帧的命令执行也重叠（发送任务在 core1，槽位满了设备自己会等）；
     *   - 主机最多领先设备一帧，排队有上限。
     * 等待有上限（默认至少 1 s），设备真的卡住时最多多排一帧，不会无限增长。 */
    const int bp_ms = g_wait_ms > 1000 ? g_wait_ms : 1000;
    flush_pending(c);
    espgpu_cmd_present(dev, 1000);
    const uint32_t fv = ++g_sh.fence;
    espgpu_cmd_fence(dev, fv);
    espgpu_cmd_flush(dev, 1000);
    if (g_sh.inflight) {
        espgpu_wait_fence(dev, g_sh.inflight_fence, bp_ms);
    }
    g_sh.inflight_fence = fv;
    g_sh.inflight = true;
    g_sh.last_frame_id = espgpu_frame_id(dev);

    /* 帧周期 */
    const LONGLONG period = g_sh.qpf.QuadPart / g_sh.fps;

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

    /* 窗口尺寸：最小化时客户区是 0×0，保留上一次的有效尺寸（视口换算靠它），也不 blit */
    bool visible = true;
    if (c->hwnd) {
        RECT rc;
        if (GetClientRect(c->hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
            if (c->win_w != rc.right - rc.left || c->win_h != rc.bottom - rc.top) {
                c->win_w = rc.right - rc.left;
                c->win_h = rc.bottom - rc.top;
                c->key_dirty = true;
            }
        } else {
            visible = false;
        }
    }
    HDC hdc = c->hdc ? c->hdc : (c->hwnd ? GetDC(c->hwnd) : nullptr);
    if (hdc && visible) {
        espgpu_blit_to_hdc(dev, hdc, c->win_w, c->win_h);
    }
    if (hdc && !c->hdc && c->hwnd) {
        ReleaseDC(c->hwnd, hdc);
    }

    g_sh.frame++;
    maybe_dump_frame(dev);

    /* ESPGPU_GL_LOG=1 时每 60 帧记一条设备侧 profile，便于在真机上定位瓶颈 */
    static bool s_perf_log = false;
    static bool s_perf_log_init = false;
    static LARGE_INTEGER s_perf_t0{};
    static uint64_t s_perf_f0 = 0;
    if (!s_perf_log_init) {
        s_perf_log = getenv("ESPGPU_GL_LOG") && !strcmp(getenv("ESPGPU_GL_LOG"), "1");
        s_perf_log_init = true;
    }
    if (s_perf_log && g_sh.frame % 60 == 0) {
        LARGE_INTEGER now2;
        QueryPerformanceCounter(&now2);
        if (s_perf_t0.QuadPart == 0) {
            s_perf_t0 = now2;
            s_perf_f0 = g_sh.frame;
        } else {
            const double secs = static_cast<double>(now2.QuadPart - s_perf_t0.QuadPart) /
                                static_cast<double>(g_sh.qpf.QuadPart);
            espgpu_stats_t ds{};
            espgpu_link_stats_t ls{};
            espgpu_get_stats(dev, &ds, 1500);
            espgpu_get_link_stats(dev, &ls);
            const double fps = secs > 0 ? (g_sh.frame - s_perf_f0) / secs : 0.0;
            log_line("perf: %.1f fps | vtx %.1f clip %.1f setup %.1f bin %.1f tile %.1f io %.1f "
                     "enc %.1f usb %.1f ms | tiles fill %u fast %u px %u gen %u passes %u "
                     "px=%u | up %.0f KB/s",
                     fps, ds.t_vtx_us / 1000.0, ds.t_clip_us / 1000.0, ds.t_setup_us / 1000.0,
                     ds.t_bin_us / 1000.0, ds.t_tile_us / 1000.0, ds.t_tile_io_us / 1000.0,
                     ds.t_encode_us / 1000.0, ds.t_usb_us / 1000.0, ds.tris_fill, ds.tris_fast,
                     ds.tris_px, ds.tris_generic, ds.raster_passes, ds.fill_pixels, ls.in_kbps);
            /* px 路径细分（issue #5）：tile 时间里段建立 / 逐像素各占多少，余下是每行 + 每三角形 */
            log_line("perf: px rows %u segs %u tested %u | seg setup %.1f pixel %.1f ms",
                     ds.px_rows, ds.px_segs, ds.px_tested, ds.t_px_seg_us / 1000.0,
                     ds.t_px_pix_us / 1000.0);
            log_line("perf: tex %u KB in %u (%u evict)", ds.tex_bytes / 1024, ds.tex_resident,
                     ds.tex_evictions);
            s_perf_t0 = now2;
            s_perf_f0 = g_sh.frame;
        }
    }

    device_poll_events();
}

} // namespace espgl
