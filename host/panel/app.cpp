/*
 * app.cpp — espgpu-panel 的外壳：Win32 窗口、D3D11、主循环、设备线程、导航
 *
 * 纯 Win32 + Direct3D 11 + Dear ImGui，不引入 GLFW/SDL。
 * 没有物理屏幕，所以 Viewer 页面就是这块 GPU 的显示器。
 */

#include "app.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "theme.h"
#include "widgets.h"

#define ESPGPU_PANEL_VERSION "0.2.0"

namespace panel {

App g_app;

namespace {

IDXGISwapChain *g_swap = nullptr;
ID3D11RenderTargetView *g_rtv = nullptr;
bool g_swap_occluded = false;
bool g_d3d_lost = false;      /* 设备移除 / 重置后等待重建 */
bool g_dx_backend = false;    /* ImGui DX11 后端是否已 Init（Shutdown 两次会空指针崩溃） */
UINT g_resize_w = 0, g_resize_h = 0; /* WM_SIZE 记下，主循环里统一 ResizeBuffers */
double g_last_activity = 0.0; /* 最近一次窗口消息（输入、尺寸、焦点…）的时间 */
std::thread g_worker;

using clock_t_ = std::chrono::steady_clock;

} // namespace

double now_s()
{
    return std::chrono::duration<double>(clock_t_::now() - g_app.t0).count();
}

/* ════════════════════════════════════════════════════════════════════
 * 日志
 * ════════════════════════════════════════════════════════════════════ */

static void push_log(int level, bool device, const char *text)
{
    /* 换行 / 控制字符会让一行变成多行高，破坏日志页 ImGuiListClipper 的等高假设 */
    std::string t(text ? text : "");
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) {
        t.pop_back();
    }
    for (char &ch : t) {
        if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
    }
    const double ts = now_s();
    std::lock_guard<std::mutex> lk(g_app.log_mu);
    g_app.log.push_back(LogLine{level, device, ts, std::move(t)});
    g_app.log_seq++;
    while (g_app.log.size() > kLogMax) {
        g_app.log.pop_front();
    }
}

void log_line(int level, bool device, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    push_log(level, device, buf);
}

/** espgpu.dll 的日志回调（接收线程上下文）。设备日志带 "device: " 前缀。 */
static void dll_log_cb(int level, const char *msg, void *)
{
    static const char kPrefix[] = "device: ";
    if (strncmp(msg, kPrefix, sizeof(kPrefix) - 1) == 0) {
        const char *body = msg + sizeof(kPrefix) - 1;
        /* ESP-IDF 日志的首字母就是级别：E/W/I/D/V */
        int lv = level;
        if (body[0] && body[1] == ' ' && body[2] == '(') {
            switch (body[0]) {
            case 'E': lv = 1; break;
            case 'W': lv = 2; break;
            case 'I': lv = 3; break;
            default: lv = 4; break;
            }
        }
        push_log(lv, true, body);
        return;
    }
    push_log(level, false, msg);
}

/* ════════════════════════════════════════════════════════════════════
 * 设置
 * ════════════════════════════════════════════════════════════════════ */

/* 设置放在 %LOCALAPPDATA%\espgpu\ 下：以前写在当前目录，从快捷方式 / Program Files
 * 启动时当前目录可能不可写或每次不同，设置就悄悄丢了。取不到时退回 exe 所在目录。 */
static std::wstring settings_path()
{
    static std::wstring cached;
    if (!cached.empty()) {
        return cached;
    }
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
        const std::wstring dir = std::wstring(buf) + L"\\espgpu";
        CreateDirectoryW(dir.c_str(), nullptr);
        const DWORD a = GetFileAttributesW(dir.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            cached = dir + L"\\espgpu-panel.cfg";
            return cached;
        }
    }
    const DWORD m = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring exe = (m > 0 && m < MAX_PATH * 2) ? std::wstring(buf, m) : std::wstring();
    const size_t slash = exe.find_last_of(L"\\/");
    cached = (slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash + 1)) +
             L"espgpu-panel.cfg";
    return cached;
}

static void load_settings()
{
    FILE *f = _wfopen(settings_path().c_str(), L"r");
    if (!f) {
        f = _wfopen(L"espgpu-panel.cfg", L"r"); /* 旧版本写在当前目录 */
    }
    if (!f) {
        return;
    }
    char line[256];
    Settings &s = g_app.settings;
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int v = 0;
        if (sscanf(line, "%63[^=]=%d", key, &v) != 2) continue;
        if (!strcmp(key, "theme")) s.theme_mode = std::clamp(v, 0, 2);
        else if (!strcmp(key, "reduce_motion")) s.reduce_motion = v != 0;
        else if (!strcmp(key, "viewer_integer")) s.viewer_integer = v != 0;
        else if (!strcmp(key, "viewer_hud")) s.viewer_hud = v != 0;
        else if (!strcmp(key, "device_log")) s.device_log = v != 0;
        else if (!strcmp(key, "log_mask")) s.log_level_mask = v & 0x0F;
        else if (!strcmp(key, "page")) s.page = std::clamp(v, 0, PAGE_COUNT - 1);
    }
    fclose(f);
}

void save_settings()
{
    g_app.want_device_log = g_app.settings.device_log;
    /* 先写临时文件再替换，写到一半崩溃 / 磁盘满时不会留下截断的配置 */
    const std::wstring path = settings_path();
    const std::wstring tmp = path + L".tmp";
    FILE *f = _wfopen(tmp.c_str(), L"w");
    if (!f) {
        return;
    }
    const Settings &s = g_app.settings;
    int ok = fprintf(f, "theme=%d\nreduce_motion=%d\nviewer_integer=%d\nviewer_hud=%d\n",
                     s.theme_mode, s.reduce_motion ? 1 : 0, s.viewer_integer ? 1 : 0,
                     s.viewer_hud ? 1 : 0) > 0;
    ok &= fprintf(f, "device_log=%d\nlog_mask=%d\npage=%d\n", s.device_log ? 1 : 0,
                  s.log_level_mask, g_app.page) > 0;
    ok &= fclose(f) == 0;
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
    }
}

/* DWM 属性（数值写死，旧 SDK 头里可能没有这些枚举） */
static void apply_dwm(bool dark)
{
    if (!g_app.hwnd) return;
    const BOOL d = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_app.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof(d));
    const int backdrop = 2; /* DWMSBT_MAINWINDOW（Mica），不支持时无副作用 */
    DwmSetWindowAttribute(g_app.hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop,
                          sizeof(backdrop));
}

static void apply_caption_color()
{
    if (!g_app.hwnd) return;
    const ImVec4 &c = theme::c().surface_container_low;
    const COLORREF cr = RGB(static_cast<int>(c.x * 255), static_cast<int>(c.y * 255),
                            static_cast<int>(c.z * 255));
    DwmSetWindowAttribute(g_app.hwnd, 35 /* DWMWA_CAPTION_COLOR */, &cr, sizeof(cr));
}

void apply_theme_settings()
{
    theme::Appearance &a = theme::appearance();
    const Settings &s = g_app.settings;
    a.follow_system = (s.theme_mode == 2);
    a.dark = a.follow_system ? theme::system_prefers_dark() : (s.theme_mode == 0);
    a.reduce_motion = s.reduce_motion;
    a.seed = 0;
    theme::set_target(a.dark, a.seed);
    apply_dwm(a.dark);
}

/* ════════════════════════════════════════════════════════════════════
 * 设备线程
 * ════════════════════════════════════════════════════════════════════ */

static void set_status(const std::string &s)
{
    std::lock_guard<std::mutex> lk(g_app.status_mu);
    g_app.status_text = s;
}

std::string status_text()
{
    std::lock_guard<std::mutex> lk(g_app.status_mu);
    return g_app.status_text;
}

std::string device_path()
{
    std::lock_guard<std::mutex> lk(g_app.status_mu);
    return g_app.device_path_;
}

static void wake_ui()
{
    if (g_app.wake) SetEvent(g_app.wake);
}

void enqueue_raw(std::function<void(espgpu_device_t *)> fn)
{
    std::lock_guard<std::mutex> lk(g_app.job_mu);
    g_app.jobs.push_back(std::move(fn));
}

bool connected()
{
    return g_app.conn.load() == Conn::Connected;
}

void enqueue(const char *what, std::function<int(espgpu_device_t *)> fn, const char *ok_msg)
{
    std::string w = what ? what : "Request";
    std::string ok = ok_msg ? ok_msg : "";
    std::lock_guard<std::mutex> lk(g_app.job_mu);
    g_app.jobs.push_back([w, ok, fn](espgpu_device_t *dev) {
        if (dev == nullptr) {
            ui::snackbar(ui::SnackKind::Error, "%s: device not connected", w.c_str());
            return;
        }
        const int rc = fn(dev);
        if (rc == 0) {
            if (!ok.empty()) ui::snackbar(ui::SnackKind::Success, "%s", ok.c_str());
            log_line(3, false, "%s: ok", w.c_str());
        } else {
            const char *why = rc == -2 ? "timed out" : rc == -1 ? "I/O error" : "device error";
            ui::snackbar(ui::SnackKind::Error, "%s failed (%s, status %d)", w.c_str(), why, rc);
            log_line(1, false, "%s failed: status %d", w.c_str(), rc);
        }
    });
}

Snapshot snapshot()
{
    Snapshot s;
    {
        std::lock_guard<std::mutex> lk(g_app.stats_mu);
        s.valid = g_app.stats_valid;
        s.s = g_app.stats;
        s.link = g_app.link;
    }
    {
        std::lock_guard<std::mutex> lk(g_app.info_mu);
        s.info = g_app.info;
        s.have_info = g_app.have_info;
    }
    return s;
}

static void worker_disconnect(const char *why)
{
    espgpu_device_t *dev = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_app.dev_mu);
        dev = g_app.dev;
        g_app.dev = nullptr;
    }
    if (dev) {
        espgpu_close(dev);
        log_line(2, false, "device closed: %s", why);
        ui::snackbar(ui::SnackKind::Error, "Device disconnected: %s", why);
    }
    {
        std::lock_guard<std::mutex> lk(g_app.stats_mu);
        g_app.stats_valid = false;
        g_app.link = espgpu_link_stats_t{};
    }
    {
        std::lock_guard<std::mutex> lk(g_app.info_mu);
        g_app.have_info = false;
    }
    g_app.conn = Conn::Searching;
    g_app.emulator = false;
    set_status("Searching for device…");
    wake_ui();
}

static void worker_try_connect()
{
    if (espgpu_device_count() <= 0) {
        g_app.conn = Conn::NotFound;
        set_status("No ESP32-S3 GPU found. Connect the board's native USB port (GPIO19/20), "
                   "not the UART port.");
        return;
    }

    char err[256] = {0};
    espgpu_device_t *dev = espgpu_open(0, err, sizeof(err));
    if (!dev) {
        g_app.conn = Conn::Busy;
        set_status(std::string("Device found but could not be opened (") + err +
                   "). WinUSB allows one client at a time - close any program using the GPU "
                   "(for example a GL app running on opengl32.dll) and the panel will reconnect.");
        return;
    }

    espgpu_device_info_t info{};
    bool have = false;
    if (const espgpu_device_info_t *ci = espgpu_cached_info(dev); ci && ci->proto_version) {
        info = *ci;
        have = true;
    } else {
        have = espgpu_get_info(dev, &info, 1500) == 0;
    }

    {
        std::lock_guard<std::mutex> lk(g_app.info_mu);
        g_app.info = info;
        g_app.have_info = have;
    }
    g_app.emulator = espgpu_is_emulator(dev);
    const std::string path = espgpu_device_path(dev) ? espgpu_device_path(dev) : "";
    {
        std::lock_guard<std::mutex> lk(g_app.status_mu);
        g_app.device_path_ = path;
    }
    {
        std::lock_guard<std::mutex> lk(g_app.dev_mu);
        g_app.dev = dev;
    }
    g_app.conn_gen++;
    g_app.conn = Conn::Connected;
    set_status(g_app.emulator ? "Connected to software emulator" : "Connected over USB");
    log_line(3, false, "connected to %s", path.c_str());
    ui::snackbar(ui::SnackKind::Success, "%s",
                 g_app.emulator ? "Connected to the software emulator" : "ESP32-S3 GPU connected");
    wake_ui();

    /* settings 只属于 UI 线程，这里读它的原子镜像 */
    if (g_app.want_device_log.load() && (info.capabilities & ESPGPU_CAP_LOG)) {
        espgpu_log_subscribe(dev, 3, 500);
    }
}

static void push_history(const espgpu_link_stats_t &ls, const espgpu_stats_t &st, bool valid)
{
    History &h = g_app.hist;
    h.fps[h.pos] = ls.fps;
    h.in_kbps[h.pos] = ls.in_kbps;
    h.out_kbps[h.pos] = ls.out_kbps;
    const bool temp_ok = valid && st.internal_temp_c10 != INT16_MIN;
    h.temp_c[h.pos] = temp_ok ? st.internal_temp_c10 / 10.0f
                              : h.temp_c[(h.pos + kHistLen - 1) % kHistLen];
    h.frame_ms[h.pos] =
        valid ? (st.t_raster_us + st.t_tnl_us + st.t_encode_us + st.t_usb_us) / 1000.0f : 0.0f;
    h.pos = (h.pos + 1) % kHistLen;
}

static void worker_main()
{
    double last_try = -10.0;
    double last_stats = 0.0;
    double last_hist = 0.0;

    while (g_app.running.load()) {
        const double now = now_s();
        espgpu_device_t *dev = g_app.dev; /* 只有本线程改写，读不需要锁 */

        /* 取一个任务（无论是否连接都要消费，未连接时任务里会报错） */
        std::function<void(espgpu_device_t *)> job;
        {
            std::lock_guard<std::mutex> lk(g_app.job_mu);
            if (!g_app.jobs.empty()) {
                job = std::move(g_app.jobs.front());
                g_app.jobs.pop_front();
            }
        }

        if (dev == nullptr) {
            if (job) job(nullptr);
            if (now - last_try >= 1.5) {
                last_try = now;
                worker_try_connect();
            }
            if (now - last_hist >= 0.2) {
                last_hist = now;
                std::lock_guard<std::mutex> lk(g_app.stats_mu);
                push_history(espgpu_link_stats_t{}, espgpu_stats_t{}, false);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        if (!espgpu_is_connected(dev)) {
            worker_disconnect("link lost");
            continue;
        }

        if (job) job(dev);

        const int got = espgpu_poll(dev, 0);
        if (got > 0) {
            g_app.new_frame = true;
            g_app.last_frame_time = now;
            if (g_app.viewer_visible.load()) wake_ui(); /* 空闲节流时让 Viewer 跟上帧率 */
        } else if (got < 0) {
            worker_disconnect("poll failed");
            continue;
        }

        espgpu_event_t ev;
        int budget = 64;
        while (budget-- > 0 && espgpu_pop_event(dev, &ev) == 1) {
            switch (ev.kind) {
            case ESPGPU_EVT_TEXTURE_EVICTED: {
                std::lock_guard<std::mutex> lk(g_app.stats_mu);
                g_app.evictions_seen++;
                break;
            }
            case ESPGPU_EVT_ERROR:
                log_line(2, true, "device error event: code=%u value=%u arg=%u", ev.code, ev.value,
                         ev.arg);
                break;
            case ESPGPU_EVT_MODE_CHANGED:
                log_line(3, true, "display mode changed");
                break;
            default:
                break;
            }
        }

        if (now - last_stats >= 0.3) {
            last_stats = now;
            espgpu_stats_t st{};
            const bool ok = espgpu_get_stats(dev, &st, 300) == 0;
            std::lock_guard<std::mutex> lk(g_app.stats_mu);
            if (ok) {
                g_app.stats = st;
            }
            g_app.stats_valid = ok;
        }

        {
            std::lock_guard<std::mutex> lk(g_app.stats_mu);
            if (const espgpu_link_stats_t *ls = espgpu_link_stats(dev)) {
                g_app.link = *ls;
            }
            if (now - last_hist >= 0.2) {
                last_hist = now;
                push_history(g_app.link, g_app.stats, g_app.stats_valid);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    worker_disconnect("panel exiting");
}

/* ════════════════════════════════════════════════════════════════════
 * D3D11
 * ════════════════════════════════════════════════════════════════════ */

static void create_rtv()
{
    ID3D11Texture2D *back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_app.d3d->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void release_rtv()
{
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

static bool init_d3d(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = 0; /* 全屏由 F11 的无边框窗口实现，不用 DXGI 独占全屏 */

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                               &g_app.d3d, &got, &g_app.ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_app.d3d, &got,
                                           &g_app.ctx);
    }
    if (FAILED(hr)) {
        return false;
    }
    /* 关掉 DXGI 默认的 Alt+Enter 独占全屏：它绕过 F11 的状态机，
     * 退出时还要求先 SetFullscreenState(FALSE) 才能释放交换链。 */
    IDXGIFactory *factory = nullptr;
    if (SUCCEEDED(g_swap->GetParent(IID_PPV_ARGS(&factory))) && factory) {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
    }
    create_rtv();
    return g_rtv != nullptr;
}

static void cleanup_d3d()
{
    release_rtv();
    if (g_app.ctx) { g_app.ctx->ClearState(); g_app.ctx->Flush(); }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_app.ctx) { g_app.ctx->Release(); g_app.ctx = nullptr; }
    if (g_app.d3d) { g_app.d3d->Release(); g_app.d3d = nullptr; }
}

static bool hr_device_lost(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

/** 驱动更新 / TDR / 拔显卡后整套重建：设备、交换链、ImGui 后端对象、帧纹理。 */
static void on_device_lost(HRESULT hr)
{
    if (!g_d3d_lost) {
        const HRESULT why = g_app.d3d ? g_app.d3d->GetDeviceRemovedReason() : hr;
        log_line(1, false, "Direct3D device lost (0x%08lX, reason 0x%08lX), recreating",
                 static_cast<unsigned long>(hr), static_cast<unsigned long>(why));
    }
    g_d3d_lost = true;
}

static bool try_recover_d3d()
{
    viewer_shutdown();
    if (g_dx_backend) {
        ImGui_ImplDX11_Shutdown();
        g_dx_backend = false;
    }
    cleanup_d3d();
    if (!init_d3d(g_app.hwnd)) {
        cleanup_d3d();
        return false;
    }
    g_dx_backend = ImGui_ImplDX11_Init(g_app.d3d, g_app.ctx); /* 字体纹理在下一次 NewFrame 里重建 */
    viewer_init();
    g_app.new_frame = true; /* 帧纹理随旧设备一起没了，下一帧重新上传 */
    g_d3d_lost = false;
    g_resize_w = g_resize_h = 0;
    log_line(3, false, "Direct3D device recreated");
    return true;
}

static void apply_pending_resize()
{
    if (g_resize_w == 0 || g_resize_h == 0 || g_swap == nullptr) {
        return;
    }
    release_rtv();
    const HRESULT hr = g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
    g_resize_w = g_resize_h = 0;
    if (hr_device_lost(hr)) {
        on_device_lost(hr);
        return;
    }
    create_rtv();
}

/* ════════════════════════════════════════════════════════════════════
 * 全屏
 * ════════════════════════════════════════════════════════════════════ */

void toggle_fullscreen()
{
    HWND hwnd = g_app.hwnd;
    if (!g_app.fullscreen) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        g_app.saved_placement.length = sizeof(WINDOWPLACEMENT);
        g_app.saved_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (GetWindowPlacement(hwnd, &g_app.saved_placement) &&
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, g_app.saved_style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_app.fullscreen = true;
            g_app.page = PAGE_VIEWER;
        }
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_app.saved_style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_app.saved_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                         SWP_FRAMECHANGED);
        g_app.fullscreen = false;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 导航与外框
 * ════════════════════════════════════════════════════════════════════ */

namespace {

struct NavItem {
    int page;
    const char *icon;
    const char *name;
    const char *title; /* 顶栏标题 */
};

const NavItem kNav[] = {
    {PAGE_OVERVIEW, theme::icon::home, "Overview", "Overview"},
    {PAGE_DISPLAY, theme::icon::display, "Display", "Display"},
    {PAGE_PERFORMANCE, theme::icon::speed, "Performance", "Performance"},
    {PAGE_VIEWER, theme::icon::eye, "Viewer", "Frame viewer"},
    {PAGE_DEBUG, theme::icon::bug, "Debug views", "Debug views"},
    {PAGE_MEMORY, theme::icon::memory, "Memory", "Memory & textures"},
    {PAGE_CLOCK, theme::icon::clock, "Clock", "Clock & temperature"},
    {PAGE_FIRMWARE, theme::icon::download, "Firmware", "Firmware update"},
    {PAGE_LOG, theme::icon::log, "Log", "Log"},
    {PAGE_APPEARANCE, theme::icon::palette, "Appearance", "Appearance"},
};

float g_page_t = 1.0f; /* 页面切换淡入进度 */

void set_page(int p)
{
    if (p != g_app.page) {
        g_app.page = p;
        g_page_t = 0.0f;
        save_settings();
    }
}

bool nav_item(const NavItem &it)
{
    using theme::px;
    const theme::Scheme &s = theme::c();
    const bool selected = (g_app.page == it.page);
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = px(52);

    ImGui::PushID(it.page);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##nav", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float sv = theme::animate(ImGui::GetID("##s"), selected ? 1.0f : 0.0f, 0.2f);
    const float hv = theme::animate(ImGui::GetID("##h"), hovered ? 1.0f : 0.0f, 0.15f);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    const float r = h * 0.5f;
    if (sv > 0.001f) {
        /* 指示条从中心向两侧展开 */
        const float half = (w * 0.5f) * (0.6f + 0.4f * sv);
        const float cx = (p.x + q.x) * 0.5f;
        dl->AddRectFilled(ImVec2(cx - half, p.y), ImVec2(cx + half, q.y),
                          theme::u32(s.secondary_container, sv), r);
    }
    if (hv > 0.001f) {
        dl->AddRectFilled(p, q, theme::u32(s.on_surface, 0.08f * hv), r);
    }
    const ImVec4 fg = theme::mix(s.on_surface_variant, s.on_secondary_container, sv);

    float x = p.x + px(16);
    if (theme::fonts().have_icons) {
        ImFont *f = theme::fonts().icon;
        const ImVec2 is = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, it.icon);
        dl->AddText(f, f->FontSize, ImVec2(x + (px(24) - is.x) * 0.5f, p.y + (h - is.y) * 0.5f),
                    theme::u32(fg), it.icon);
        x += px(24) + px(12);
    }
    ImFont *tf = selected ? theme::fonts().body_strong : theme::fonts().body;
    const ImVec2 ts = tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0, it.name);
    dl->AddText(tf, tf->FontSize, ImVec2(x, p.y + (h - ts.y) * 0.5f), theme::u32(fg), it.name);
    ImGui::PopID();
    return clicked;
}

void draw_nav()
{
    using theme::px;
    const theme::Scheme &s = theme::c();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), theme::u32(s.surface_container_low),
                      px(16), ImDrawFlags_RoundCornersRight);

    ImGui::SetCursorPos(ImVec2(px(12), px(20)));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(-1);

    /* 头部：品牌标志 + 名称 */
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float d = px(40);
        dl->AddRectFilled(ImVec2(p.x + px(8), p.y), ImVec2(p.x + px(8) + d, p.y + d),
                          theme::u32(s.primary_container), px(12));
        if (theme::fonts().have_icons) {
            ImFont *f = theme::fonts().icon;
            const ImVec2 is = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, theme::icon::chip);
            dl->AddText(f, f->FontSize,
                        ImVec2(p.x + px(8) + (d - is.x) * 0.5f, p.y + (d - is.y) * 0.5f),
                        theme::u32(s.on_primary_container), theme::icon::chip);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + px(8) + d + px(12), p.y - px(1)));
        ImGui::BeginGroup();
        ImGui::PushFont(theme::fonts().body_strong);
        ImGui::TextUnformatted("ESP32-S3 GPU");
        ImGui::PopFont();
        ui::text_label("Control Panel v" ESPGPU_PANEL_VERSION);
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + d + px(20)));
    }

    const float item_w = ws.x - px(24);
    ImGui::BeginChild("##navitems", ImVec2(item_w, ws.y - px(20) - px(60) - px(150)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    auto header = [&](const char *t) {
        ImGui::SetCursorPosX(px(16));
        ImGui::Dummy(ImVec2(0, px(2)));
        ImGui::SetCursorPosX(px(16));
        ui::text_label(t);
    };
    header("DEVICE");
    for (int i = 0; i < 4; i++) {
        if (nav_item(kNav[i])) set_page(kNav[i].page);
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    header("TOOLS");
    for (int i = 4; i < 9; i++) {
        if (nav_item(kNav[i])) set_page(kNav[i].page);
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    header("SETTINGS");
    if (nav_item(kNav[9])) set_page(kNav[9].page);
    ImGui::EndChild();

    /* 底部：连接状态卡片 */
    ImGui::SetCursorPos(ImVec2(px(12), ws.y - px(140)));
    ImGui::BeginGroup();
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 q(p.x + item_w, p.y + px(120));
        dl->AddRectFilled(p, q, theme::u32(s.surface_container_high), px(16));
        ImGui::SetCursorScreenPos(ImVec2(p.x + px(16), p.y + px(14)));
        ImGui::BeginGroup();
        const Conn cn = g_app.conn.load();
        ImVec4 dot = s.outline;
        const char *label = "Searching";
        if (cn == Conn::Connected) {
            dot = g_app.emulator ? s.tertiary : s.success;
            label = g_app.emulator ? "Emulator" : "Connected";
        } else if (cn == Conn::Busy) {
            dot = s.warning;
            label = "Device busy";
        } else if (cn == Conn::NotFound) {
            dot = s.error;
            label = "Not found";
        }
        ui::status_dot(dot);
        ImGui::SameLine(0, px(8));
        ImGui::PushFont(theme::fonts().body_strong);
        ImGui::TextUnformatted(label);
        ImGui::PopFont();
        if (cn == Conn::Connected) {
            const Snapshot sn = snapshot();
            ImGui::PushFont(theme::fonts().title);
            ImGui::Text("%.1f", sn.link.fps);
            ImGui::PopFont();
            ImGui::SameLine(0, px(4));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(4));
            ui::text_label("fps");
            char buf[64];
            snprintf(buf, sizeof(buf), "%.0f KB/s in  \xc2\xb7  %.0f KB/s out", sn.link.in_kbps,
                     sn.link.out_kbps);
            ui::text_label(buf);
        } else {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + item_w - px(32));
            ui::text_label(cn == Conn::Busy ? "Another app holds the device"
                                            : "Plug in the native USB port");
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
    }
    ImGui::EndGroup();

    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

void draw_top_bar(const char *title)
{
    using theme::px;
    const theme::Scheme &s = theme::c();
    const float h = px(64);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    ImGui::SetCursorScreenPos(ImVec2(p.x + px(24), p.y + (h - theme::fonts().title->FontSize) * 0.5f));
    ImGui::PushFont(theme::fonts().title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();

    /* 右侧：状态 chip + 主题切换 */
    const Snapshot sn = snapshot();
    char chip_txt[96];
    const Conn cn = g_app.conn.load();
    ImVec4 bg = s.surface_container_highest, fg = s.on_surface_variant;
    if (cn == Conn::Connected) {
        snprintf(chip_txt, sizeof(chip_txt), "%s  \xc2\xb7  %.1f fps",
                 g_app.emulator ? "Emulator" : "USB", sn.link.fps);
        bg = g_app.emulator ? s.tertiary_container : s.success_container;
        fg = g_app.emulator ? s.on_tertiary_container : s.on_success_container;
    } else if (cn == Conn::Busy) {
        snprintf(chip_txt, sizeof(chip_txt), "Device busy");
        bg = s.warning_container;
        fg = s.on_warning_container;
    } else {
        snprintf(chip_txt, sizeof(chip_txt), "Disconnected");
        bg = s.error_container;
        fg = s.on_error_container;
    }
    ImFont *lf = theme::fonts().label;
    const float cw = lf->CalcTextSizeA(lf->FontSize, FLT_MAX, 0, chip_txt).x + px(24) +
                     (theme::fonts().have_icons ? px(22) : 0);
    const float right = p.x + w - px(24);
    ImGui::SetCursorScreenPos(ImVec2(right - px(40) - px(12) - cw, p.y + (h - px(28)) * 0.5f));
    ui::chip(chip_txt, bg, fg, cn == Conn::Connected ? theme::icon::usb : theme::icon::warning);

    ImGui::SetCursorScreenPos(ImVec2(right - px(40), p.y + (h - px(40)) * 0.5f));
    const bool dark = theme::appearance().dark;
    if (ui::icon_button("theme", dark ? theme::icon::brightness : theme::icon::moon,
                        dark ? "Switch to light theme" : "Switch to dark theme")) {
        g_app.settings.theme_mode = dark ? 1 : 0;
        apply_theme_settings();
        save_settings();
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(0, 0));
}

void draw_frame_ui()
{
    using theme::px;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (g_app.fullscreen) {
        /* 全屏：只有画面 */
        viewer_draw(ImGui::GetContentRegionAvail(), g_app.settings.viewer_hud);
        ImGui::End();
        return;
    }

    const float nav_w = px(256);
    ImGui::BeginChild("##nav", ImVec2(nav_w, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    draw_nav();
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##main", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    const char *title = "";
    for (const NavItem &it : kNav) {
        if (it.page == g_app.page) title = it.title;
    }
    draw_top_bar(title);

    /* 页面淡入 + 轻微上移 */
    if (g_page_t < 1.0f) {
        g_page_t += ImGui::GetIO().DeltaTime / 0.2f;
        if (theme::appearance().reduce_motion || g_page_t > 1.0f) g_page_t = 1.0f;
    }
    const float e = theme::ease_emphasized(g_page_t);

    ImGui::SetCursorPosX(px(24));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.25f + 0.75f * e);
    ImGui::BeginChild("##page", ImVec2(ImGui::GetContentRegionAvail().x - px(24), 0),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(10) * (1.0f - e));
    switch (g_app.page) {
    case PAGE_OVERVIEW: page_overview(); break;
    case PAGE_DISPLAY: page_display(); break;
    case PAGE_PERFORMANCE: page_performance(); break;
    case PAGE_VIEWER: page_viewer(); break;
    case PAGE_DEBUG: page_debug(); break;
    case PAGE_MEMORY: page_memory(); break;
    case PAGE_CLOCK: page_clock(); break;
    case PAGE_FIRMWARE: page_firmware(); break;
    case PAGE_LOG: page_log(); break;
    case PAGE_APPEARANCE: page_appearance(); break;
    default: break;
    }
    ImGui::Dummy(ImVec2(0, px(24)));
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::End();
}

} // namespace

bool require_device()
{
    if (connected()) {
        return true;
    }
    const Conn cn = g_app.conn.load();
    const std::string st = status_text();
    if (cn == Conn::Busy) {
        ui::empty_state(theme::icon::warning, "Device busy", st.c_str());
    } else if (cn == Conn::NotFound) {
        ui::empty_state(theme::icon::usb, "Device not connected", st.c_str());
    } else {
        ui::empty_state(theme::icon::refresh, "Connecting…",
                        "Looking for an ESP32-S3 GPU on USB. Set ESPGPU_EMULATOR=1 to use the "
                        "software emulator instead.");
    }
    return false;
}

/* ════════════════════════════════════════════════════════════════════
 * WndProc
 * ════════════════════════════════════════════════════════════════════ */

} // namespace panel

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace panel;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* 任何窗口消息（输入、尺寸、焦点、主题…）都让主循环恢复全速一小段时间 */
    if (msg != WM_PAINT && msg != WM_TIMER) {
        g_last_activity = now_s();
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        return 1;
    }

    switch (msg) {
    case WM_SIZE:
        /* 最小化 / 0 尺寸时不动交换链（ResizeBuffers(0,0) 会失败，RTV 变空）；
         * 实际的 ResizeBuffers 推迟到主循环，拖动边框时不会每条消息都重建一次 */
        if (wp != SIZE_MINIMIZED && LOWORD(lp) > 0 && HIWORD(lp) > 0) {
            g_resize_w = static_cast<UINT>(LOWORD(lp));
            g_resize_h = static_cast<UINT>(HIWORD(lp));
        }
        return 0;
    case WM_DPICHANGED: {
        g_app.pending_dpi = static_cast<float>(HIWORD(wp)) / 96.0f;
        const RECT *r = reinterpret_cast<const RECT *>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *mm = reinterpret_cast<MINMAXINFO *>(lp);
        mm->ptMinTrackSize.x = static_cast<LONG>(960 * g_app.dpi_scale);
        mm->ptMinTrackSize.y = static_cast<LONG>(620 * g_app.dpi_scale);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t *>(lp), L"ImmersiveColorSet") == 0 &&
            g_app.settings.theme_mode == 2) {
            apply_theme_settings();
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) {
            toggle_fullscreen();
            return 0;
        }
        if (wp == VK_ESCAPE && g_app.fullscreen) {
            toggle_fullscreen();
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ════════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════════ */

static void rebuild_fonts(float scale)
{
    g_app.dpi_scale = scale;
    theme::set_dpi(scale);
    theme::load_fonts(scale);
    theme::apply_style_sizes(scale);
    if (g_dx_backend) {
        ImGui_ImplDX11_InvalidateDeviceObjects(); /* NewFrame 里会重建字体纹理 */
    }
}

/** 等待下一次需要绘制：窗口消息、worker 的唤醒（新帧 / 连接变化）或超时。 */
static void wait_for_work(DWORD timeout_ms)
{
    MsgWaitForMultipleObjectsEx(g_app.wake ? 1 : 0, g_app.wake ? &g_app.wake : nullptr,
                                timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int)
{
    ImGui_ImplWin32_EnableDpiAwareness();

    load_settings();
    g_app.page = g_app.settings.page;
    g_app.want_device_log = g_app.settings.device_log;
    g_app.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    espgpu_set_log_callback(dll_log_cb, nullptr);
    espgpu_set_log_level(3);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"ESPGPU_PANEL";
    RegisterClassExW(&wc);

    /* 初始窗口按主显示器 DPI 缩放 */
    const POINT origin{0, 0};
    const float boot_scale =
        ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ESP32-S3 GPU Control Panel",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              static_cast<int>(1320 * boot_scale), static_cast<int>(840 * boot_scale),
                              nullptr, nullptr, hinst, nullptr);
    if (hwnd == nullptr) {
        return 1;
    }
    g_app.hwnd = hwnd;

    if (!init_d3d(hwnd)) {
        MessageBoxW(hwnd, L"Direct3D 11 initialization failed.", L"ESP32-S3 GPU", MB_ICONERROR);
        cleanup_d3d();
        UnregisterClassW(wc.lpszClassName, hinst);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    /* 所有窗口都是 NoSavedSettings / 每帧定位，ini 里没有有用的东西；
     * 以前写进当前目录，从别处启动时会到处留文件 */
    io.IniFilename = nullptr;

    ImGui_ImplWin32_Init(hwnd);
    g_dx_backend = ImGui_ImplDX11_Init(g_app.d3d, g_app.ctx);

    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    g_app.dpi_scale = scale;
    theme::set_dpi(scale);
    theme::load_fonts(scale);
    theme::apply_style_sizes(scale);

    /* 先立即应用一次主题（不做过渡），再显示窗口，避免白闪 */
    {
        const bool rm = g_app.settings.reduce_motion;
        g_app.settings.reduce_motion = true;
        apply_theme_settings();
        theme::update(0.0f);
        g_app.settings.reduce_motion = rm;
        theme::appearance().reduce_motion = rm;
    }
    apply_caption_color();

    viewer_init();

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    log_line(3, false, "espgpu-panel " ESPGPU_PANEL_VERSION " started");
    g_worker = std::thread(worker_main);

    ImVec4 last_caption{};
    bool running = true;
    bool idle_ok = false;  /* 上一帧之后是否允许空闲等待 */
    double last_recover = -10.0;
    while (running) {
        /* 空闲节流：最近 0.75 s 内没有任何窗口消息、没有 snackbar 时，不再每个 vsync
         * 都重画（144/240 Hz 屏上能省下整颗核），而是等消息 / 新帧 / 100 ms 超时。
         * 0.75 s 覆盖了所有过渡动画（≤0.25 s）与 tooltip 延迟（0.4 s）。 */
        if (idle_ok) {
            wait_for_work(100);
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        if (g_d3d_lost) {
            const double t = now_s();
            bool ok = false;
            if (t - last_recover >= 1.0) { /* 每秒重试一次 */
                last_recover = t;
                ok = try_recover_d3d();
            }
            if (!ok) {
                wait_for_work(100);
                idle_ok = false;
                continue;
            }
        }

        /* 最小化 / 被完全遮挡：不画，等消息 */
        if (IsIconic(hwnd) ||
            (g_swap_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
            wait_for_work(100);
            idle_ok = false;
            continue;
        }
        g_swap_occluded = false;

        apply_pending_resize();
        if (g_d3d_lost || g_rtv == nullptr) {
            if (!g_d3d_lost) on_device_lost(S_OK); /* RTV 建不出来：整套重建 */
            wait_for_work(100);
            idle_ok = false;
            continue;
        }

        if (g_app.pending_dpi > 0.0f) {
            rebuild_fonts(g_app.pending_dpi);
            g_app.pending_dpi = 0.0f;
        }

        /* 只在画面可见时上传：不可见时新帧标志一直保留，切到 Viewer 的那一帧立即补传 */
        const bool viewer_vis = g_app.fullscreen || g_app.page == PAGE_VIEWER;
        g_app.viewer_visible = viewer_vis;
        if (viewer_vis && g_app.new_frame.exchange(false)) {
            viewer_upload();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        theme::update(io.DeltaTime);
        ImGui::NewFrame();

        draw_frame_ui();
        ui::snackbar_draw();

        ImGui::Render();

        const theme::Scheme &s = theme::c();
        if (memcmp(&last_caption, &s.surface_container_low, sizeof(ImVec4)) != 0) {
            last_caption = s.surface_container_low;
            apply_caption_color();
        }
        const float clear[4] = {s.surface.x, s.surface.y, s.surface.z, 1.0f};
        g_app.ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_app.ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT hr = g_swap->Present(1, 0);
        g_swap_occluded = (hr == DXGI_STATUS_OCCLUDED);
        if (hr_device_lost(hr)) {
            on_device_lost(hr);
        }

        idle_ok = now_s() - g_last_activity > 0.75 && !ui::snackbar_active() &&
                  !io.WantTextInput && g_app.pending_dpi <= 0.0f;
    }

    save_settings();

    /* 窗口此时已经销毁（WM_DESTROY），等 worker 收尾时不会显示成“未响应”。
     * OTA 在每个 8 KB 块之后检查取消；其余控制请求最长 1.5 s 超时。 */
    g_app.ota.cancel = true;
    g_app.running = false;
    if (g_worker.joinable()) {
        g_worker.join();
    }
    /* 设备已关闭，之后 DLL 不会再回调；解除回调，避免静态析构后的悬空 g_app */
    espgpu_set_log_callback(nullptr, nullptr);

    viewer_shutdown();
    if (g_dx_backend) ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanup_d3d();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hinst);
    if (g_app.wake) CloseHandle(g_app.wake);
    return 0;
}
