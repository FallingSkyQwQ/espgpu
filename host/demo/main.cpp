/*
 * espgpu-demo — M2 的 2D 渲染演示
 *
 * 目的不是"好看"，而是把光栅器的每条路径都压一遍：
 *   - RGB565 纹理 + 最近邻采样
 *   - REPEAT 环绕（滚动背景）
 *   - RGBA4444 纹理 + 双线性 + alpha 混合（旋转精灵）
 *   - 无纹理纯色四边形
 *   - scissor 裁剪
 *
 * 画面画在自己的窗口里（WinUSB 独占：demo 运行时 espgpu-panel 连不上设备，
 * 以前提示"去面板的 Viewer 页看"其实看不到）。Esc / 关窗口 / Ctrl+C 退出。
 */

#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "espgpu/espgpu.h"

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib") /* MinGW 由 CMake 链接 winmm */
#endif

namespace {

/* Ctrl+C：让主循环正常退出（关设备、恢复计时器精度），而不是被直接杀掉 */
std::atomic<bool> g_stop{false};

BOOL WINAPI on_console_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

/* ── 窗口：只用来显示设备回传的帧 ─────────────────────────────────── */

HWND g_hwnd = nullptr;
bool g_repaint = false; /* WM_PAINT / 尺寸变化后补画一次，不等下一帧 */

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        g_stop = true;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        g_repaint = true;
        return 0;
    }
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

void pump_messages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void blit(espgpu_device_t *dev)
{
    if (!g_hwnd) return;
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    if (cr.right <= cr.left || cr.bottom <= cr.top) return; /* 最小化 */
    HDC hdc = GetDC(g_hwnd);
    espgpu_blit_to_hdc(dev, hdc, cr.right - cr.left, cr.bottom - cr.top);
    ReleaseDC(g_hwnd, hdc);
}

constexpr uint16_t kWidth = 320;
constexpr uint16_t kHeight = 200;
constexpr int kTargetFps = 24;

uint16_t rgb565(int r, int g, int b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

inline int16_t fixed16(float px)
{
    return static_cast<int16_t>(px * 16.0f);
}

inline int16_t tex17(float t)
{
    return static_cast<int16_t>(t * 16.0f);
}

/* 生成一张棋盘格背景纹理，带一点噪声，避免压缩率好得不真实 */
std::vector<uint16_t> make_checker(int w, int h)
{
    std::vector<uint16_t> px(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const bool on = ((x / 16) + (y / 16)) & 1;
            const int n = ((x * 7 + y * 13) % 9) - 4;
            px[static_cast<size_t>(y) * w + x] =
                on ? rgb565(90 + n, 100 + n, 120 + n) : rgb565(40 + n, 46 + n, 60 + n);
        }
    }
    return px;
}

/* 圆角软边精灵，RGBA4444 */
std::vector<uint16_t> make_sprite(int w, int h)
{
    std::vector<uint16_t> px(static_cast<size_t>(w) * h);
    const float cx = (w - 1) * 0.5f;
    const float cy = (h - 1) * 0.5f;
    const float r = (w < h ? w : h) * 0.5f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const float dx = x - cx;
            const float dy = y - cy;
            float d = std::sqrt(dx * dx + dy * dy) / r;
            if (d > 1.0f) {
                d = 1.0f;
            }
            /* 边缘 20% 渐变到透明 */
            float a = (d < 0.8f) ? 1.0f : (1.0f - (d - 0.8f) / 0.2f);
            if (a < 0.0f) {
                a = 0.0f;
            }

            const int rr = static_cast<int>(80 + 170 * (1.0f - d));
            const int gg = static_cast<int>(200 - 120 * d);
            const int bb = static_cast<int>(120 + 100 * d);

            const uint32_t r4 = static_cast<uint32_t>(rr * 15 / 255);
            const uint32_t g4 = static_cast<uint32_t>(gg * 15 / 255);
            const uint32_t b4 = static_cast<uint32_t>(bb * 15 / 255);
            const uint32_t a4 = static_cast<uint32_t>(a * 15.0f);
            px[static_cast<size_t>(y) * w + x] =
                static_cast<uint16_t>((r4 << 12) | (g4 << 8) | (b4 << 4) | a4);
        }
    }
    return px;
}

void push_quad(std::vector<espgpu_vertex2d_t> &out, float x[4], float y[4], float u[4],
               float v[4], uint16_t color)
{
    for (int i = 0; i < 4; i++) {
        espgpu_vertex2d_t vert{};
        vert.x = fixed16(x[i]);
        vert.y = fixed16(y[i]);
        vert.u = tex17(u[i]);
        vert.v = tex17(v[i]);
        vert.color = color;
        out.push_back(vert);
    }
}

/* 把收到的帧写成 24 位 BMP，用于离线确认光栅器输出（面板之外的第二双眼睛） */
bool dump_bmp(espgpu_device_t *dev, const char *path)
{
    const int w = espgpu_frame_width(dev);
    const int h = espgpu_frame_height(dev);
    if (w <= 0 || h <= 0) {
        printf("dump: no frame yet (%dx%d)\n", w, h);
        return false;
    }

    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    if (espgpu_frame_snapshot(dev, rgba.data(), rgba.size(), nullptr, nullptr, nullptr) != 0) {
        printf("dump: snapshot failed (cap=%zu)\n", rgba.size());
        return false;
    }

    const int row_bytes = (w * 3 + 3) & ~3;
    const int data_size = row_bytes * h;
    const int file_size = 54 + data_size;

    FILE *f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) {
        return false;
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
            row[x * 3 + 0] = p[2]; /* B */
            row[x * 3 + 1] = p[1]; /* G */
            row[x * 3 + 2] = p[0]; /* R */
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    printf("frame dumped to %s (%dx%d)\n", path, w, h);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    espgpu_set_log_level(2);

    /* Windows 默认计时器精度 15.6ms，sleep_for(34ms) 会被量化成 46.8ms，
     * 直接把 24fps 的节奏压成 20fps。提到 1ms。 */
    timeBeginPeriod(1);

    long max_frames = 0;
    bool stress_scroll = false;
    int drawcalls = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atol(argv[++i]);
        } else if (strcmp(argv[i], "--stress") == 0) {
            /* 全屏滚动：每帧每个像素都变，差分压缩完全失效，
             * 用来测链路上限，不代表真实负载。 */
            stress_scroll = true;
        } else if (strcmp(argv[i], "--drawcalls") == 0 && i + 1 < argc) {
            /* M2 验收：N 个小精灵，每个一次独立 drawcall（绑定 + 状态 + 绘制） */
            drawcalls = atoi(argv[++i]);
        }
    }

    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    char err[256] = {0};
    espgpu_device_t *dev = espgpu_open(0, err, sizeof(err));
    if (dev == nullptr) {
        printf("open failed: %s\n", err);
        return 1;
    }

    /* 固定 320x200 RGB565；编码用 delta+RLE，和实测最优组合一致 */
    espgpu_mode_t mode{};
    mode.width = kWidth;
    mode.height = kHeight;
    mode.format = static_cast<uint8_t>(ESPGPU_FMT_RGB565);
    mode.codec = espgpu_preferred_codec(dev);
    mode.max_fps = kTargetFps;
    mode.vsync = 1;
    espgpu_set_mode(dev, &mode, 0, 1000);

    /* 纹理：id 1 背景，id 2 精灵 */
    auto bg = make_checker(64, 64);
    auto sprite = make_sprite(48, 48);

    espgpu_cmd_upload_texture(dev, 1, 64, 64, ESPGPU_FMT_RGB565, bg.data());
    espgpu_cmd_upload_texture(dev, 2, 48, 48, ESPGPU_FMT_RGBA4444, sprite.data());
    espgpu_cmd_present(dev, 1000);

    printf("espgpu-demo: %ux%u @ %d fps, %d extra drawcalls, Ctrl+C to stop\n", kWidth, kHeight,
           kTargetFps, drawcalls);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"espgpu_demo2d";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT wr{0, 0, kWidth * 3, kHeight * 3};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"espgpu-demo (ESP32-S3 GPU, 2D)", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
                           nullptr, nullptr, wc.hInstance, nullptr);
    if (g_hwnd) ShowWindow(g_hwnd, SW_SHOW);

    std::vector<espgpu_vertex2d_t> verts;
    verts.reserve(4096);

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t frames = 0;
    float angle = 0.0f;
    float scroll = 0.0f;
    double t_present = 0.0, t_sleep = 0.0;

    /* 帧率控制用绝对截止时间：原来按 1000/24 = 41 ms（整数截断）相对每帧起点睡，
     * 节奏偏快且 sleep 的超调逐帧累积；这里每帧截止时间 += 1/24 s，落后超过一帧就重新对齐 */
    const auto period = std::chrono::microseconds(1000000 / kTargetFps);
    auto deadline = std::chrono::steady_clock::now() + period;

    while (!g_stop.load()) {

        verts.clear();

        /* scissor 是跨帧保持的状态（与 GL 一致），每帧开头必须显式复位，
         * 否则上一帧的进度条裁剪区会把整帧都裁掉。 */
        espgpu_cmd_scissor(dev, 0, 0, kWidth, kHeight);

        /* 1. 背景：2x2 个四边形铺满。
         * 默认是静态的（GPU 的典型负载：大部分像素不变，只有局部在动），
         * 这样 delta+RLE 才能把整帧压到几 KB。--stress 打开滚动，
         * 用于测链路上限。 */
        {
            const float uv0 = stress_scroll ? scroll : 0.0f;
            const float uv1 = uv0 + 2.0f * 64.0f;
            for (int gy = 0; gy < 2; gy++) {
                for (int gx = 0; gx < 2; gx++) {
                    float x[4] = {gx * 160.0f, (gx + 1) * 160.0f, (gx + 1) * 160.0f,
                                  gx * 160.0f};
                    float y[4] = {gy * 100.0f, gy * 100.0f, (gy + 1) * 100.0f,
                                  (gy + 1) * 100.0f};
                    float u[4] = {uv0, uv1, uv1, uv0};
                    float v[4] = {uv0, uv0, uv1, uv1};
                    push_quad(verts, x, y, u, v, 0xFFFF);
                }
            }
            espgpu_cmd_set_pipeline(dev, ESPGPU_BLEND_NONE, ESPGPU_FILTER_NEAREST,
                                    ESPGPU_WRAP_REPEAT, 0, 0);
            espgpu_cmd_bind_texture(dev, 1, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT);
            espgpu_cmd_draw_quads(dev, verts.data(), static_cast<uint32_t>(verts.size() / 4));
            verts.clear();
        }

        /* 2. 旋转的 alpha 精灵，双线性采样 */
        {
            const float cx = kWidth * 0.5f + std::cos(angle * 0.7f) * 70.0f;
            const float cy = kHeight * 0.5f + std::sin(angle) * 40.0f;
            const float s = 30.0f;
            const float cs = std::cos(angle);
            const float sn = std::sin(angle);

            const float sx[4] = {-1, 1, 1, -1};
            const float sy[4] = {-1, -1, 1, 1};
            float x[4], y[4];
            for (int i = 0; i < 4; i++) {
                x[i] = cx + (sx[i] * cs - sy[i] * sn) * s;
                y[i] = cy + (sx[i] * sn + sy[i] * cs) * s;
            }
            float u[4] = {0, 48.0f, 48.0f, 0};
            float v[4] = {0, 0, 48.0f, 48.0f};

            push_quad(verts, x, y, u, v, 0xFFFF);
            espgpu_cmd_set_pipeline(dev, ESPGPU_BLEND_ALPHA, ESPGPU_FILTER_LINEAR,
                                    ESPGPU_WRAP_CLAMP, 1, 8);
            espgpu_cmd_bind_texture(dev, 2, ESPGPU_FILTER_LINEAR, ESPGPU_WRAP_CLAMP);
            espgpu_cmd_draw_quads(dev, verts.data(), static_cast<uint32_t>(verts.size() / 4));
            verts.clear();
        }

        /* 2b. drawcall 压力：N 个 8x8 精灵绕圈，每个单独设置状态与纹理 */
        for (int i = 0; i < drawcalls; i++) {
            const float a = angle * 0.5f + static_cast<float>(i) * 0.0125f * 6.2831853f;
            const float r = 20.0f + static_cast<float>(i % 50) * 1.6f;
            const float cx = kWidth * 0.5f + std::cos(a) * r * 1.6f;
            const float cy = kHeight * 0.5f + std::sin(a) * r;
            float x[4] = {cx - 4, cx + 4, cx + 4, cx - 4};
            float y[4] = {cy - 4, cy - 4, cy + 4, cy + 4};
            const bool spr = (i & 1) != 0;
            const float tw = spr ? 48.0f : 16.0f;
            float u[4] = {0, tw, tw, 0};
            float v[4] = {0, 0, tw, tw};
            push_quad(verts, x, y, u, v, 0xFFFF);
            espgpu_cmd_set_pipeline(dev, spr ? ESPGPU_BLEND_ALPHA : ESPGPU_BLEND_NONE,
                                    ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT, spr ? 1 : 0, 8);
            espgpu_cmd_bind_texture(dev, spr ? 2 : 1, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT);
            espgpu_cmd_draw_quads(dev, verts.data(), 1);
            verts.clear();
        }

        /* 3. 纯色四边形 + scissor：右上角一个进度条 */
        {
            espgpu_cmd_scissor(dev, 200, 10, 110, 12);
            espgpu_cmd_set_pipeline(dev, ESPGPU_BLEND_NONE, ESPGPU_FILTER_NEAREST,
                                    ESPGPU_WRAP_CLAMP, 0, 0);
            espgpu_cmd_bind_texture(dev, 0, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_CLAMP);

            const float t = static_cast<float>(frames % 240) / 240.0f;
            float x[4] = {200, 200 + 110.0f * t, 200 + 110.0f * t, 200};
            float y[4] = {10, 10, 22, 22};
            float u[4] = {0, 0, 0, 0};
            float v[4] = {0, 0, 0, 0};
            push_quad(verts, x, y, u, v, rgb565(0x40, 0xC0, 0xFF));
            espgpu_cmd_draw_quads(dev, verts.data(), 1);
            verts.clear();
        }

        /* 4. 推进动画并提交 */
        angle += 0.035f;
        if (stress_scroll) {
            /* uv 是 1/16 texel 的 int16（±2048 texel）：不回绕的话约 2 分钟后溢出成乱码。
             * 纹理 64 texel 且 REPEAT，按 64 取模画面不变 */
            scroll = std::fmod(scroll + 0.6f, 64.0f);
        }

        const auto tp0 = std::chrono::steady_clock::now();
        espgpu_cmd_present(dev, 1000);
        const auto tp1 = std::chrono::steady_clock::now();
        t_present += std::chrono::duration<double, std::milli>(tp1 - tp0).count();
        frames++;

        /* 帧率控制 */
        {
            const auto ts0 = std::chrono::steady_clock::now();
            if (ts0 < deadline) {
                std::this_thread::sleep_until(deadline);
                t_sleep += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - ts0)
                               .count();
                deadline += period;
            } else if (ts0 - deadline > period) {
                deadline = ts0 + period; /* 落后超过一帧：不追帧，从现在重新计 */
            } else {
                deadline += period; /* 略晚：不睡，下一帧补回来 */
            }
        }

        if (!espgpu_is_connected(dev)) {
            printf("device lost\n");
            break;
        }
        pump_messages();
        if (espgpu_poll(dev, 0) > 0 || g_repaint) {
            g_repaint = false;
            blit(dev);
        }

        if (frames % (kTargetFps * 4) == 0) {
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const espgpu_link_stats_t *ls = espgpu_link_stats(dev);
            const double n = static_cast<double>(kTargetFps * 4);
            /* 统计窗口在 espgpu_poll 里滚动，所以打印必须放在 poll 之后，
             * 否则读到的是刚被清零的窗口。 */
            printf("frames %llu  %.1f fps  %.0f KB/s  decode %.2f ms | resync %u lost %u\n"
                   "    host: present %.1f ms  sleep %.1f ms  (per frame, last %d)\n",
                   static_cast<unsigned long long>(frames), frames / secs, ls->in_kbps,
                   ls->decode_ms, ls->resyncs, ls->fragments_lost, t_present / n, t_sleep / n,
                   kTargetFps * 4);
            fflush(stdout);
            t_present = t_sleep = 0.0;
        }

        /* 第 96 帧导出一次，方便离线检查画面 */
        if (frames == 96) {
            dump_bmp(dev, "demo-frame.bmp");
        }

        if (max_frames > 0 && static_cast<long>(frames) >= max_frames) {
            printf("done: %ld frames\n", max_frames);
            break;
        }
    }

    if (g_hwnd && IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
    espgpu_close(dev);
    timeEndPeriod(1);
    return 0;
}
