/*
 * espgpu-demo3d — M5 的 3D 验收演示：旋转立方体 + 地形 + 光照
 *
 * 覆盖设备端 T&L 的每条路径：
 *   - 地形：32x32 高度图，顶点 / 索引一次性上传到设备端缓冲（DATA chunk），
 *     之后每帧只发一条 DRAW(buffer)，下行带宽几乎为零（PLAN R6 的显示列表思路）
 *   - 顶点光照（法线 + NORMAL 矩阵 + 方向光 + 环境光）、线性雾、透视校正纹理
 *   - 立方体：每帧内联顶点，背面剔除 + 深度测试 + 双线性
 *   - 一条线框坐标轴（线图元）
 *
 * 画面画在自己的窗口里（WinUSB 独占，面板这时连不上设备）。
 * 没有硬件时设 ESPGPU_EMULATOR=1 用软件模拟器。
 *
 *   espgpu-demo3d.exe [--frames N] [--fps N] [--res 320x200]
 */

#include <windows.h>
#include <timeapi.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "espgpu/espgpu.h"

namespace {

struct Mat4 {
    float m[16]; /* 行主序，clip = M * v */
};

Mat4 identity()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 mul(const Mat4 &a, const Mat4 &b)
{
    Mat4 r{};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) {
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            }
            r.m[i * 4 + j] = s;
        }
    }
    return r;
}

Mat4 perspective(float fovy, float aspect, float n, float f)
{
    Mat4 r{};
    const float t = 1.0f / std::tan(fovy * 0.5f);
    r.m[0] = t / aspect;
    r.m[5] = t;
    r.m[10] = (f + n) / (n - f);
    r.m[11] = 2.0f * f * n / (n - f);
    r.m[14] = -1.0f;
    return r;
}

Mat4 translate(float x, float y, float z)
{
    Mat4 r = identity();
    r.m[3] = x;
    r.m[7] = y;
    r.m[11] = z;
    return r;
}

Mat4 rot_y(float a)
{
    Mat4 r = identity();
    r.m[0] = std::cos(a);
    r.m[2] = std::sin(a);
    r.m[8] = -std::sin(a);
    r.m[10] = std::cos(a);
    return r;
}

Mat4 rot_x(float a)
{
    Mat4 r = identity();
    r.m[5] = std::cos(a);
    r.m[6] = -std::sin(a);
    r.m[9] = std::sin(a);
    r.m[10] = std::cos(a);
    return r;
}

/** 相机：绕原点公转、俯视 */
Mat4 look_at(float ex, float ey, float ez, float cx, float cy, float cz)
{
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    fx /= fl; fy /= fl; fz /= fl;
    /* s = f × up(0,1,0) */
    float sx = -fz, sy = 0.0f, sz = fx;
    float sl = std::sqrt(sx * sx + sz * sz);
    sx /= sl; sz /= sl;
    /* u = s × f */
    const float ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;
    Mat4 r = identity();
    r.m[0] = sx; r.m[1] = sy; r.m[2] = sz;
    r.m[4] = ux; r.m[5] = uy; r.m[6] = uz;
    r.m[8] = -fx; r.m[9] = -fy; r.m[10] = -fz;
    r.m[3] = -(sx * ex + sy * ey + sz * ez);
    r.m[7] = -(ux * ex + uy * ey + uz * ez);
    r.m[11] = (fx * ex + fy * ey + fz * ez);
    return r;
}

/** 法线矩阵：刚体变换的左上 3x3 就是它自己的逆转置 */
Mat4 normal_matrix(const Mat4 &mv)
{
    Mat4 r = identity();
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r.m[i * 4 + j] = mv.m[i * 4 + j];
        }
    }
    return r;
}

uint16_t rgb565(int r, int g, int b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* 顶点：位置 F32x3 + 法线 I8x3(归一化) + 纹理坐标 F32x2 = 28 字节 */
#pragma pack(push, 1)
struct Vtx {
    float x, y, z;
    int8_t nx, ny, nz, pad;
    float u, v;
};
#pragma pack(pop)

espgpu_vertex_layout_t make_layout(uint8_t index_type)
{
    espgpu_vertex_layout_t l{};
    l.stride = sizeof(Vtx);
    l.attrib_count = 3;
    l.index_type = index_type;
    l.attribs[0] = {ESPGPU_ATTR_POSITION, ESPGPU_TYPE_F32, 3, 0, 0, 0, 0};
    l.attribs[1] = {ESPGPU_ATTR_NORMAL, ESPGPU_TYPE_I8, 3, 1, 12, 0, 0};
    l.attribs[2] = {ESPGPU_ATTR_TEXCOORD0, ESPGPU_TYPE_F32, 2, 0, 16, 0, 0};
    return l;
}

float height_at(float x, float z)
{
    return 0.6f * std::sin(x * 0.45f) * std::cos(z * 0.38f) + 0.25f * std::sin(x * 1.3f + z * 0.9f) -
           0.8f;
}

/* ── 窗口 ─────────────────────────────────────────────────────────── */

HWND g_hwnd = nullptr;
bool g_quit = false;

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        g_quit = true;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(h);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

} // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    espgpu_set_log_level(2);
    timeBeginPeriod(1);

    long max_frames = 0;
    int fps = 24;
    int W = 320, H = 200;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            max_frames = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--res") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &W, &H);
        }
    }
    if (fps < 1) fps = 24;

    char err[256] = {0};
    espgpu_device_t *dev = espgpu_open(0, err, sizeof(err));
    if (dev == nullptr) {
        printf("open failed: %s\n(hint: set ESPGPU_EMULATOR=1 to run without hardware)\n", err);
        return 1;
    }

    espgpu_mode_t mode{};
    mode.width = static_cast<uint16_t>(W);
    mode.height = static_cast<uint16_t>(H);
    mode.format = ESPGPU_FMT_RGB565;
    mode.codec = ESPGPU_CODEC_DELTA_RLE;
    mode.max_fps = static_cast<uint8_t>(fps);
    mode.vsync = 1;
    espgpu_set_mode(dev, &mode, 0, 1500);

    /* 窗口 */
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"espgpu_demo3d";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT rc{0, 0, W * 3, H * 3};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"espgpu-demo3d (ESP32-S3 GPU)", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                           nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);

    /* 纹理：草地 / 岩石混合（地形），棋盘 + 边框（立方体） */
    std::vector<uint16_t> grass(64 * 64), crate(32 * 32);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            const int n = ((x * 37 + y * 91) ^ (x * y)) % 23;
            const bool rock = ((x / 16 + y / 16) & 1) && ((x ^ y) & 4);
            grass[y * 64 + x] = rock ? rgb565(110 + n, 100 + n, 90 + n) : rgb565(50 + n, 120 + n * 2, 40 + n);
        }
    }
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            const bool edge = x < 2 || y < 2 || x > 29 || y > 29 || x == y || x == 31 - y;
            crate[y * 32 + x] = edge ? rgb565(90, 60, 30) : rgb565(200, 150, 80);
        }
    }
    espgpu_cmd_upload_texture_ex(dev, 1, 64, 64, ESPGPU_FMT_RGB565, grass.data(), ESPGPU_TEXF_PRIORITY);
    espgpu_cmd_upload_texture_ex(dev, 2, 32, 32, ESPGPU_FMT_RGB565, crate.data(), ESPGPU_TEXF_PRIORITY);

    /* 地形 → 设备端缓冲 1：[顶点][u16 索引] */
    constexpr int N = 32;
    constexpr float kSize = 16.0f;
    std::vector<Vtx> tv(N * N);
    for (int z = 0; z < N; z++) {
        for (int x = 0; x < N; x++) {
            const float fx = (static_cast<float>(x) / (N - 1) - 0.5f) * kSize;
            const float fz = (static_cast<float>(z) / (N - 1) - 0.5f) * kSize;
            const float e = 0.05f;
            const float dx = (height_at(fx + e, fz) - height_at(fx - e, fz)) / (2 * e);
            const float dz = (height_at(fx, fz + e) - height_at(fx, fz - e)) / (2 * e);
            float nx = -dx, ny = 1.0f, nz = -dz;
            const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
            Vtx &v = tv[z * N + x];
            v.x = fx;
            v.y = height_at(fx, fz);
            v.z = fz;
            v.nx = static_cast<int8_t>(nx / l * 127);
            v.ny = static_cast<int8_t>(ny / l * 127);
            v.nz = static_cast<int8_t>(nz / l * 127);
            v.u = static_cast<float>(x) * 0.25f;
            v.v = static_cast<float>(z) * 0.25f;
        }
    }
    std::vector<uint16_t> ti;
    for (int z = 0; z + 1 < N; z++) {
        for (int x = 0; x + 1 < N; x++) {
            const uint16_t a = static_cast<uint16_t>(z * N + x), b = a + 1;
            const uint16_t c = static_cast<uint16_t>(a + N), d = c + 1;
            /* 从上方看逆时针 */
            ti.insert(ti.end(), {a, c, b, b, c, d});
        }
    }
    std::vector<uint8_t> blob(tv.size() * sizeof(Vtx) + ti.size() * 2);
    memcpy(blob.data(), tv.data(), tv.size() * sizeof(Vtx));
    memcpy(blob.data() + tv.size() * sizeof(Vtx), ti.data(), ti.size() * 2);
    if (espgpu_buffer_upload(dev, 1, blob.data(), static_cast<uint32_t>(blob.size())) != 0) {
        printf("terrain buffer upload failed\n");
    }

    /* 立方体（24 顶点，每面独立法线与纹理坐标） */
    std::vector<Vtx> cube;
    std::vector<uint16_t> ci;
    {
        const float s = 0.7f;
        const int faces[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        for (int f = 0; f < 6; f++) {
            const float n[3] = {static_cast<float>(faces[f][0]), static_cast<float>(faces[f][1]),
                                static_cast<float>(faces[f][2])};
            /* 面内两个切向 */
            float t1[3] = {n[1], n[2], n[0]};
            float t2[3] = {n[1] * t1[2] - n[2] * t1[1], n[2] * t1[0] - n[0] * t1[2],
                           n[0] * t1[1] - n[1] * t1[0]};
            const float cs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            const uint16_t base = static_cast<uint16_t>(cube.size());
            for (int k = 0; k < 4; k++) {
                Vtx v{};
                v.x = (n[0] + t1[0] * cs[k][0] + t2[0] * cs[k][1]) * s;
                v.y = (n[1] + t1[1] * cs[k][0] + t2[1] * cs[k][1]) * s;
                v.z = (n[2] + t1[2] * cs[k][0] + t2[2] * cs[k][1]) * s;
                v.nx = static_cast<int8_t>(n[0] * 127);
                v.ny = static_cast<int8_t>(n[1] * 127);
                v.nz = static_cast<int8_t>(n[2] * 127);
                v.u = (cs[k][0] + 1) * 0.5f;
                v.v = (cs[k][1] + 1) * 0.5f;
                cube.push_back(v);
            }
            /* t2 = n × t1，(t1, t2) 从外看是逆时针 */
            ci.insert(ci.end(), {base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
                                 base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)});
        }
    }

    const espgpu_vertex_layout_t layout = make_layout(1);
    const Mat4 proj = perspective(1.0f, static_cast<float>(W) / H, 0.3f, 40.0f);

    printf("espgpu-demo3d: %dx%d @ %d fps%s, Esc to quit\n", W, H, fps,
           espgpu_is_emulator(dev) ? " (software emulator)" : "");

    const auto t_start = std::chrono::steady_clock::now();
    uint64_t frames = 0;
    uint32_t last_id = espgpu_frame_id(dev);

    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) {
            break;
        }
        const auto f0 = std::chrono::steady_clock::now();
        const float t = std::chrono::duration<float>(f0 - t_start).count();

        /* 相机绕场景公转 */
        const float ca = t * 0.25f;
        const Mat4 view = look_at(std::cos(ca) * 7.0f, 3.2f, std::sin(ca) * 7.0f, 0, 0, 0);

        espgpu_cmd_viewport(dev, 0, 0, static_cast<int16_t>(W), static_cast<int16_t>(H));
        espgpu_cmd_scissor(dev, 0, 0, static_cast<int16_t>(W), static_cast<int16_t>(H));
        espgpu_cmd_clear_ex(dev, 3, rgb565(120, 150, 200), 0xFFFF);

        /* 方向光：世界空间里从右上方照来，变换到眼空间 */
        float ld[3] = {0.5f, 0.8f, 0.3f};
        const float ll = std::sqrt(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
        espgpu_light_t light{};
        for (int i = 0; i < 3; i++) {
            light.dir[i] = (view.m[i * 4 + 0] * ld[0] + view.m[i * 4 + 1] * ld[1] +
                            view.m[i * 4 + 2] * ld[2]) / ll;
        }
        light.diffuse[0] = 0.85f;
        light.diffuse[1] = 0.8f;
        light.diffuse[2] = 0.7f;
        const float ambient[3] = {0.25f, 0.27f, 0.32f};
        espgpu_cmd_lighting(dev, true, ambient, &light, 1);
        espgpu_cmd_color(dev, 255, 255, 255, 255);

        espgpu_pipeline_t p{};
        p.mask = 0xFFFF & ~ESPGPU_PS_TEXTURE;
        p.depth_test = 1;
        p.depth_write = 1;
        p.depth_func = ESPGPU_CMP_LEQUAL;
        p.alpha_func = ESPGPU_CMP_ALWAYS;
        p.cull_mode = ESPGPU_CULL_BACK;
        p.front_face = ESPGPU_FRONT_CCW;
        p.shade_mode = ESPGPU_SHADE_TEX_MOD_COLOR;
        p.fog_enable = 1;
        p.fog_mode = ESPGPU_FOG_LINEAR;
        p.fog_color = rgb565(120, 150, 200);
        espgpu_pipeline_fog_t fog{6.0f, 18.0f, 1.0f};
        espgpu_pipeline_ext_t ext{};
        ext.shade_model = ESPGPU_SHADE_MODEL_SMOOTH;
        ext.color_write = 1;
        ext.lighting = 1;
        espgpu_cmd_pipeline(dev, &p, &fog, &ext);
        espgpu_cmd_vertex_layout(dev, &layout);

        /* 地形（设备端缓冲） */
        {
            const Mat4 mv = view;
            const Mat4 mvp = mul(proj, mv);
            const Mat4 nm = normal_matrix(mv);
            espgpu_cmd_matrix(dev, ESPGPU_MATRIX_MVP, mvp.m);
            espgpu_cmd_matrix(dev, ESPGPU_MATRIX_MODELVIEW, mv.m);
            espgpu_cmd_matrix(dev, ESPGPU_MATRIX_NORMAL, nm.m);
            espgpu_cmd_bind_texture_unit(dev, 0, 1, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT);
            espgpu_cmd_draw_buffer(dev, ESPGPU_PRIM_TRIANGLES, 1, 0, N * N,
                                   static_cast<uint32_t>(tv.size() * sizeof(Vtx)),
                                   static_cast<uint32_t>(ti.size()));
        }

        /* 旋转立方体（内联顶点，双线性） */
        {
            const Mat4 model = mul(translate(0.0f, 0.6f + 0.25f * std::sin(t * 1.7f), 0.0f),
                                   mul(rot_y(t * 0.9f), rot_x(t * 0.6f)));
            const Mat4 mv = mul(view, model);
            const Mat4 mvp = mul(proj, mv);
            const Mat4 nm = normal_matrix(mv);
            espgpu_cmd_matrix(dev, ESPGPU_MATRIX_MVP, mvp.m);
            espgpu_cmd_matrix(dev, ESPGPU_MATRIX_MODELVIEW, mv.m);
            espgpu_cmd_matrix(dev, ESPGPU_MATRIX_NORMAL, nm.m);
            espgpu_cmd_bind_texture_unit(dev, 0, 2, ESPGPU_FILTER_LINEAR, ESPGPU_WRAP_CLAMP);
            espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLES, cube.data(), static_cast<uint32_t>(cube.size()),
                            sizeof(Vtx), ci.data(), static_cast<uint32_t>(ci.size()), 1);
        }

        /* 坐标轴（线，不打光、不雾化、不测深度） */
        {
            espgpu_pipeline_t lp = p;
            lp.shade_mode = ESPGPU_SHADE_FLAT;
            lp.fog_enable = 0;
            lp.depth_test = 0;
            lp.cull_mode = ESPGPU_CULL_NONE;
            espgpu_pipeline_ext_t lext = ext;
            lext.lighting = 0;
            lext.line_width = 1;
            espgpu_cmd_pipeline(dev, &lp, &fog, &lext);
            const Mat4 mvp = mul(proj, view);
            espgpu_cmd_matrix(dev, ESPGPU_MATRIX_MVP, mvp.m);
            Vtx axis[2] = {};
            axis[0].x = 0; axis[0].y = 2.2f; axis[0].z = 0;
            axis[1].x = 0; axis[1].y = 3.4f; axis[1].z = 0;
            espgpu_cmd_color(dev, 255, 60, 60, 255);
            espgpu_cmd_draw(dev, ESPGPU_PRIM_LINES, axis, 2, sizeof(Vtx), nullptr, 0, 1);
        }

        espgpu_cmd_present(dev, 1000);
        frames++;

        /* 帧同步：等这一帧回来再画，最多等一个帧周期多一点（背压，PLAN R5） */
        espgpu_wait_frame(dev, last_id, 1000 / fps + 30);
        last_id = espgpu_frame_id(dev);
        espgpu_poll(dev, 0);

        RECT cr;
        GetClientRect(g_hwnd, &cr);
        HDC hdc = GetDC(g_hwnd);
        espgpu_blit_to_hdc(dev, hdc, cr.right - cr.left, cr.bottom - cr.top);
        ReleaseDC(g_hwnd, hdc);

        /* 纹理被设备换出就重传（本 demo 纹理带 PRIORITY，正常不会发生） */
        espgpu_event_t ev;
        while (espgpu_pop_event(dev, &ev)) {
            if (ev.kind == ESPGPU_EVT_TEXTURE_EVICTED) {
                if (ev.arg == 1) {
                    espgpu_cmd_upload_texture_ex(dev, 1, 64, 64, ESPGPU_FMT_RGB565, grass.data(), ESPGPU_TEXF_PRIORITY);
                } else if (ev.arg == 2) {
                    espgpu_cmd_upload_texture_ex(dev, 2, 32, 32, ESPGPU_FMT_RGB565, crate.data(), ESPGPU_TEXF_PRIORITY);
                }
            } else if (ev.kind == ESPGPU_EVT_BUFFER_EVICTED && ev.arg == 1) {
                espgpu_buffer_upload(dev, 1, blob.data(), static_cast<uint32_t>(blob.size()));
            }
        }

        if (frames % (static_cast<uint64_t>(fps) * 4) == 0) {
            const espgpu_link_stats_t *ls = espgpu_link_stats(dev);
            espgpu_stats_t ds{};
            const bool have = espgpu_get_stats(dev, &ds, 200) == 0;
            printf("frames %llu  %.1f fps  up %.0f KB/s  down %.0f KB/s",
                   static_cast<unsigned long long>(frames), ls->fps, ls->in_kbps, ls->out_kbps);
            if (have) {
                printf(" | tris %u culled %u  tnl %.1f ms  raster %.1f ms  enc %.1f ms  usb %.1f ms",
                       ds.triangles, ds.culled, ds.t_tnl_us / 1000.0, ds.t_raster_us / 1000.0,
                       ds.t_encode_us / 1000.0, ds.t_usb_us / 1000.0);
            }
            printf("\n");
        }

        const auto el = std::chrono::steady_clock::now() - f0;
        const auto period = std::chrono::microseconds(1000000 / fps);
        if (el < period) {
            std::this_thread::sleep_for(period - el);
        }
        if (!espgpu_is_connected(dev)) {
            printf("device lost\n");
            break;
        }
        if (max_frames > 0 && static_cast<long>(frames) >= max_frames) {
            break;
        }
    }

    espgpu_close(dev);
    timeEndPeriod(1);
    return 0;
}
