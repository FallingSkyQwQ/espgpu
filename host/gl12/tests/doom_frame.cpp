/*
 * doom_frame.cpp — 仿 prboom-plus / DSDA-Doom GL 渲染器的调用流，喂给 host/gl12，
 * 统计每帧下行字节 / 命令数，并用 firmware/core 渲染出 BMP，数「裂缝」像素（清屏色露出来的点）。
 *
 * 场景：N×N 个 128 单位的格子，每格随机地板 / 天花板高度、光照；相邻格高差处有上 / 下墙，
 * 外圈有整墙。场景封闭，任何露出清屏色（品红）的像素都是裂缝。
 *   --lists   地板用显示列表（gl_use_display_lists）
 *   --jitter  顶点带随机小数（GL 节点那种 16.16 坐标）
 *   --nofog   不开雾
 *   --sub N / --irr  地板拆成 N×N 个（不规则的）子扇形。注意子扇形在格子边上加点会形成 T 型接缝，
 *             这两种模式下的「裂缝」主要是场景本身的 T 型接缝（真实 Doom 地图也有，prboom 的 gl_seamless 处理），
 *             用来比较改动前后即可，不应要求为 0
 */
#include "gl_context.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <chrono>
#include <string>
#include <vector>

using namespace espgl;

extern "C" {
HGLRC WINAPI ewglCreateContext(HDC hdc);
BOOL WINAPI ewglMakeCurrent(HDC hdc, HGLRC h);
BOOL WINAPI ewglSwapBuffers(HDC hdc);
BOOL WINAPI ewglDeleteContext(HGLRC h);
}

struct FakeStats {
    uint64_t raw = 0, wire = 0, chunks = 0;
    uint64_t op_bytes[32] = {}, op_count[32] = {};
    uint64_t data_bytes = 0;
    uint64_t draw_verts = 0;
};
extern FakeStats g_fs;
extern int g_split_draws;
extern std::vector<uint16_t> g_fake_frame;
extern int g_fake_fw, g_fake_fh;
extern bool g_fake_render;
void fake_reset_stats();
void fake_write_bmp(const char *path);
const char *fake_op_name(int op);

namespace {

constexpr float MAP_SCALE = 128.0f;
int N = 10;
int SUB = 1;
bool g_irr = false; /* 不规则子扇形（随机内点 + 边上随机加点），更像真实地图 */ /* 每格地板拆成 SUB×SUB 个子扇形（类似 Doom 的 subsector） */
bool g_sortlight = false, g_lists = false, g_jitter = false, g_fog = true, g_hud = true;

struct Cell {
    int floor_h, ceil_h, light, ftex, ctex;
    GLuint list_f = 0, list_c = 0;
};
std::vector<Cell> cells;
std::vector<float> cx, cy; /* 角点坐标（地图单位），(N+1)^2 */
GLuint wall_tex[24], flat_tex[8], spr_tex[6], hud_tex[12];

float corner_x(int i, int j) { return cx[size_t(j) * (N + 1) + i]; }
float corner_y(int i, int j) { return cy[size_t(j) * (N + 1) + i]; }

void make_tex(GLuint name, int w, int h, int seed, bool holes)
{
    std::vector<uint8_t> px(size_t(w) * h * 4);
    std::mt19937 rng(seed);
    const uint8_t base_r = uint8_t(rng() % 200 + 30), base_g = uint8_t(rng() % 200 + 30), base_b = uint8_t(rng() % 200 + 30);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            const int v = int((x / 8 + y / 8) & 1) * 40 + int(rng() % 8) * 4; /* 少量颜色（≤256） */
            p[0] = uint8_t(std::min(255, base_r + v));
            p[1] = uint8_t(std::min(255, base_g + v / 2));
            p[2] = uint8_t(std::min(255, base_b + v / 3));
            p[3] = 255;
            if (holes) {
                const float dx = (x - w * 0.5f) / (w * 0.5f), dy = (y - h * 0.5f) / (h * 0.5f);
                if (dx * dx + dy * dy > 0.8f) p[3] = 0;
            }
        }
    }
    impl_glBindTexture(GL_TEXTURE_2D, name);
    impl_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    impl_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    impl_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
}

void build_map()
{
    std::mt19937 rng(1234);
    cells.resize(size_t(N) * N);
    for (auto &c : cells) {
        c.floor_h = int(rng() % 4) * 16;
        c.ceil_h = 128 + int(rng() % 4) * 24;
        static const int L[] = {96, 128, 160, 192, 224, 255};
        c.light = L[rng() % 6];
        c.ftex = int(rng() % 8);
        c.ctex = int(rng() % 8);
    }
    cx.resize(size_t(N + 1) * (N + 1));
    cy.resize(cx.size());
    std::uniform_real_distribution<float> jr(0.0f, 1.0f);
    for (int j = 0; j <= N; j++) {
        for (int i = 0; i <= N; i++) {
            float x = i * 128.0f - N * 64.0f, y = j * 128.0f - N * 64.0f;
            if (g_jitter) {
                /* 16.16 定点小数，与 GL 节点顶点一样 */
                x += std::floor(jr(rng) * 65536.0f) / 65536.0f * 7.0f;
                y += std::floor(jr(rng) * 65536.0f) / 65536.0f * 7.0f;
            }
            cx[size_t(j) * (N + 1) + i] = x;
            cy[size_t(j) * (N + 1) + i] = y;
        }
    }
}

float light_of(int l) { return l / 255.0f; }
float fog_of(int l) { return g_fog ? (255 - l + 16) / 1024.0f * 0.5f : 0.0f; }

float g_cur_fog = -1;
bool g_fog_on = false;
GLuint g_last_tex = 0;

void set_fog(float d)
{
    if (d > 0) {
        if (!g_fog_on) {
            impl_glEnable(GL_FOG);
            g_fog_on = true;
        }
        if (d != g_cur_fog) {
            impl_glFogf(GL_FOG_DENSITY, d);
            g_cur_fog = d;
        }
    } else if (g_fog_on) {
        impl_glDisable(GL_FOG);
        g_fog_on = false;
    }
}

void bind(GLuint t)
{
    if (t != g_last_tex) {
        impl_glBindTexture(GL_TEXTURE_2D, t);
        g_last_tex = t;
    }
}

/* 一格的地板（或天花板）：SUB×SUB 个子格，每个子格是中心 + 4 角的扇形（6 顶点，y=0，由 glTranslatef 抬高） */
void flat_verts(int i, int j, bool ceil, std::vector<float> &out)
{
    out.clear();
    auto lerp = [&](float a, float b, float t) { return a + (b - a) * t; };
    const float x0 = corner_x(i, j), y0 = corner_y(i, j);
    const float x1 = corner_x(i + 1, j), y1 = corner_y(i + 1, j);
    const float x2 = corner_x(i + 1, j + 1), y2 = corner_y(i + 1, j + 1);
    const float x3 = corner_x(i, j + 1), y3 = corner_y(i, j + 1);
    auto at = [&](float u, float v, float &x, float &y) {
        x = lerp(lerp(x0, x1, u), lerp(x3, x2, u), v);
        y = lerp(lerp(y0, y1, u), lerp(y3, y2, u), v);
    };
    if (g_irr) {
        /* 每格拆成 SUB*SUB 个不规则扇形：随机内点 + 边上随机多点 */
        std::mt19937 rng(uint32_t(i * 7919 + j * 104729 + (ceil ? 1 : 0)));
        std::uniform_real_distribution<float> U(0.15f, 0.85f);
        for (int sj = 0; sj < SUB; sj++) {
            for (int si = 0; si < SUB; si++) {
                const float u0 = float(si) / SUB, u1 = float(si + 1) / SUB, v0 = float(sj) / SUB, v1 = float(sj + 1) / SUB;
                std::vector<std::pair<float, float>> ring;
                const float cu[4] = {u0, u1, u1, u0}, cv[4] = {v0, v0, v1, v1};
                for (int e = 0; e < 4; e++) {
                    ring.push_back({cu[e], cv[e]});
                    const int extra = int(rng() % 3);
                    std::vector<float> ts;
                    for (int k = 0; k < extra; k++) ts.push_back(U(rng));
                    std::sort(ts.begin(), ts.end());
                    for (float t : ts) ring.push_back({lerp(cu[e], cu[(e + 1) % 4], t), lerp(cv[e], cv[(e + 1) % 4], t)});
                }
                std::vector<std::pair<float, float>> P;
                P.push_back({lerp(u0, u1, U(rng)), lerp(v0, v1, U(rng))});
                for (auto &r : ring) P.push_back(r);
                P.push_back(ring[0]);
                const int n = int(P.size());
                for (int k = 0; k < n; k++) {
                    const int kk = ceil ? (k == 0 ? 0 : n - k) : k;
                    float x, y;
                    at(P[kk].first, P[kk].second, x, y);
                    out.push_back(x / MAP_SCALE);
                    out.push_back(0);
                    out.push_back(y / MAP_SCALE);
                    out.push_back(x / 64.0f);
                    out.push_back(y / 64.0f);
                }
                out.push_back(-1e30f); /* 扇形分隔 */
                out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);
            }
        }
        return;
    }
    for (int sj = 0; sj < SUB; sj++) {
        for (int si = 0; si < SUB; si++) {
            float P[6][2];
            const float u0 = float(si) / SUB, u1 = float(si + 1) / SUB, v0 = float(sj) / SUB, v1 = float(sj + 1) / SUB;
            at((u0 + u1) * 0.5f, (v0 + v1) * 0.5f, P[0][0], P[0][1]);
            at(u0, v0, P[1][0], P[1][1]);
            at(u1, v0, P[2][0], P[2][1]);
            at(u1, v1, P[3][0], P[3][1]);
            at(u0, v1, P[4][0], P[4][1]);
            P[5][0] = P[1][0];
            P[5][1] = P[1][1];
            for (int k = 0; k < 6; k++) {
                const int kk = ceil ? (k == 0 ? 0 : 6 - k) : k; /* 天花板反向，保持朝下的绕序 */
                out.push_back(P[kk][0] / MAP_SCALE);
                out.push_back(0);
                out.push_back(P[kk][1] / MAP_SCALE);
                out.push_back(P[kk][0] / 64.0f);
                out.push_back(P[kk][1] / 64.0f);
            }
        }
    }
}

std::vector<float> g_flat_va;

void draw_flat_arrays(const std::vector<float> &v)
{
    impl_glEnableClientState(GL_VERTEX_ARRAY);
    impl_glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    impl_glVertexPointer(3, GL_FLOAT, 20, v.data());
    impl_glTexCoordPointer(2, GL_FLOAT, 20, v.data() + 3);
    if (g_irr) {
        size_t start = 0;
        for (size_t k = 0; k < v.size() / 5; k++) {
            if (v[k * 5] == -1e30f) {
                impl_glDrawArrays(GL_TRIANGLE_FAN, GLint(start), GLsizei(k - start));
                start = k + 1;
            }
        }
    } else {
        for (size_t f = 0; f < v.size() / 30; f++) impl_glDrawArrays(GL_TRIANGLE_FAN, GLint(f * 6), 6);
    }
    impl_glDisableClientState(GL_VERTEX_ARRAY);
    impl_glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

void init_lists()
{
    if (!g_lists) return;
    GLuint base = impl_glGenLists(N * N * 2);
    std::vector<float> v;
    for (int j = 0; j < N; j++) {
        for (int i = 0; i < N; i++) {
            Cell &c = cells[size_t(j) * N + i];
            c.list_f = base++;
            flat_verts(i, j, false, v);
            impl_glNewList(c.list_f, GL_COMPILE);
            draw_flat_arrays(v);
            impl_glEndList();
            c.list_c = base++;
            flat_verts(i, j, true, v);
            impl_glNewList(c.list_c, GL_COMPILE);
            draw_flat_arrays(v);
            impl_glEndList();
        }
    }
}

struct Wall {
    float x1, y1, x2, y2, zb, zt, len;
    int tex, light;
};

void collect_walls(std::vector<Wall> &w)
{
    std::mt19937 rng(77);
    auto add = [&](float x1, float y1, float x2, float y2, int zb, int zt, int light) {
        if (zt <= zb) return;
        Wall a{x1, y1, x2, y2, float(zb), float(zt), std::hypot(x2 - x1, y2 - y1), int(rng() % 24), light};
        w.push_back(a);
    };
    for (int j = 0; j < N; j++) {
        for (int i = 0; i < N; i++) {
            const Cell &c = cells[size_t(j) * N + i];
            /* 每条边由「左/下」那一格负责：看向邻居 */
            struct E { int di, dj; int ai, aj, bi, bj; };
            const E edges[4] = {{1, 0, 1, 0, 1, 1}, {0, 1, 1, 1, 0, 1}, {-1, 0, 0, 1, 0, 0}, {0, -1, 0, 0, 1, 0}};
            for (const E &e : edges) {
                const int ni = i + e.di, nj = j + e.dj;
                const float x1 = corner_x(i + e.ai, j + e.aj), y1 = corner_y(i + e.ai, j + e.aj);
                const float x2 = corner_x(i + e.bi, j + e.bj), y2 = corner_y(i + e.bi, j + e.bj);
                if (ni < 0 || nj < 0 || ni >= N || nj >= N) {
                    add(x1, y1, x2, y2, c.floor_h, c.ceil_h, c.light); /* 外墙 */
                    continue;
                }
                const Cell &n = cells[size_t(nj) * N + ni];
                add(x1, y1, x2, y2, c.floor_h, n.floor_h, c.light); /* 下墙：邻居地板更高 */
                add(x1, y1, x2, y2, n.ceil_h, c.ceil_h, c.light);   /* 上墙：邻居天花板更低 */
            }
        }
    }
}

void draw_wall(const Wall &w)
{
    bind(wall_tex[w.tex]);
    const float l = light_of(w.light);
    impl_glColor4f(l, l, l, 1.0f);
    set_fog(fog_of(w.light));
    const float ul = 0, ur = w.len / 64.0f, vt = 0, vb = (w.zt - w.zb) / 128.0f;
    impl_glBegin(GL_TRIANGLE_FAN);
    impl_glTexCoord2f(ul, vb); impl_glVertex3f(w.x1 / MAP_SCALE, w.zb / MAP_SCALE, w.y1 / MAP_SCALE);
    impl_glTexCoord2f(ul, vt); impl_glVertex3f(w.x1 / MAP_SCALE, w.zt / MAP_SCALE, w.y1 / MAP_SCALE);
    impl_glTexCoord2f(ur, vt); impl_glVertex3f(w.x2 / MAP_SCALE, w.zt / MAP_SCALE, w.y2 / MAP_SCALE);
    impl_glTexCoord2f(ur, vb); impl_glVertex3f(w.x2 / MAP_SCALE, w.zb / MAP_SCALE, w.y2 / MAP_SCALE);
    impl_glEnd();
}

void infinite_perspective(float fovy, float aspect, float znear)
{
    const float f = 1.0f / std::tan(fovy * 3.14159265f / 360.0f);
    float m[16] = {};
    m[0] = f / aspect;
    m[5] = f;
    m[10] = -1.0f;
    m[11] = -1.0f;
    m[14] = -2.0f * znear;
    impl_glLoadMatrixf(m);
}

std::vector<Wall> g_walls;
float cam_x = 0, cam_y = 0, cam_z = 41, cam_yaw = 30;

void draw_frame(HDC hdc)
{
    impl_glViewport(0, 0, 640, 400);
    impl_glClearColor(1, 0, 1, 1);
    impl_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    impl_glMatrixMode(GL_PROJECTION);
    impl_glLoadIdentity();
    infinite_perspective(64.0f, 1.6f, 0.05f);
    impl_glMatrixMode(GL_MODELVIEW);
    impl_glLoadIdentity();
    impl_glRotatef(0, 0, 0, 1);
    impl_glRotatef(0, 1, 0, 0);
    impl_glRotatef(cam_yaw, 0, 1, 0);
    impl_glTranslatef(-cam_x / MAP_SCALE, -cam_z / MAP_SCALE, -cam_y / MAP_SCALE);

    impl_glEnable(GL_DEPTH_TEST);
    impl_glDepthFunc(GL_LEQUAL);
    impl_glEnable(GL_ALPHA_TEST);
    impl_glAlphaFunc(GL_GEQUAL, 0.5f);
    impl_glShadeModel(GL_FLAT);
    impl_glEnable(GL_TEXTURE_2D);
    impl_glDisable(GL_BLEND);
    impl_glFogi(GL_FOG_MODE, GL_EXP);
    const float fc[4] = {0, 0, 0, 0};
    impl_glFogfv(GL_FOG_COLOR, fc);
    g_cur_fog = -1;
    g_last_tex = 0;

    /* 地板 / 天花板：按纹理排序 */
    for (int pass = 0; pass < 2; pass++) {
        for (int t = 0; t < 8 * 256; t++) {
            if (!g_sortlight && t >= 8) break;
            for (int j = 0; j < N; j++) {
                for (int i = 0; i < N; i++) {
                    const Cell &c = cells[size_t(j) * N + i];
                    if (g_sortlight ? ((pass == 0 ? c.ftex : c.ctex) != t / 256 || c.light != t % 256) : (pass == 0 ? c.ftex : c.ctex) != t) continue;
                    bind(flat_tex[g_sortlight ? t / 256 : t]);
                    const float l = light_of(c.light);
                    impl_glColor4f(l, l, l, 1.0f);
                    set_fog(fog_of(c.light));
                    impl_glPushMatrix();
                    impl_glTranslatef(0, (pass == 0 ? c.floor_h : c.ceil_h) / MAP_SCALE, 0);
                    if (g_lists) {
                        impl_glCallList(pass == 0 ? c.list_f : c.list_c);
                    } else {
                        flat_verts(i, j, pass == 1, g_flat_va);
                        draw_flat_arrays(g_flat_va);
                    }
                    impl_glPopMatrix();
                }
            }
        }
    }
    /* 墙：按纹理排序 */
    for (int t = 0; t < 24; t++) {
        for (int L = 0; L < 256; L++) {
            for (const Wall &w : g_walls) {
                if (w.tex == t && (!g_sortlight || w.light == L)) draw_wall(w);
            }
            if (!g_sortlight) break;
        }
    }
    /* 精灵：alpha 测试 */
    std::mt19937 rng(5);
    for (int s = 0; s < 40; s++) {
        const float sx = float(int(rng() % (N * 128)) - N * 64), sy = float(int(rng() % (N * 128)) - N * 64);
        const int ci = std::clamp(int((sx + N * 64) / 128), 0, N - 1), cj = std::clamp(int((sy + N * 64) / 128), 0, N - 1);
        const Cell &c = cells[size_t(cj) * N + ci];
        bind(spr_tex[s % 6]);
        const float l = light_of(c.light);
        impl_glColor4f(l, l, l, 1.0f);
        set_fog(fog_of(c.light));
        const float a = -cam_yaw * 3.14159265f / 180.0f;
        const float ux = std::cos(a) * 16, uy = std::sin(a) * 16;
        const float z0 = float(c.floor_h), z1 = z0 + 56;
        if (s % 7 == 3) {
            impl_glEnable(GL_BLEND);
            impl_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            impl_glColor4f(l, l, l, 0.5f);
        }
        impl_glBegin(GL_TRIANGLE_STRIP);
        impl_glTexCoord2f(0, 0); impl_glVertex3f((sx - ux) / MAP_SCALE, z1 / MAP_SCALE, (sy - uy) / MAP_SCALE);
        impl_glTexCoord2f(1, 0); impl_glVertex3f((sx + ux) / MAP_SCALE, z1 / MAP_SCALE, (sy + uy) / MAP_SCALE);
        impl_glTexCoord2f(0, 1); impl_glVertex3f((sx - ux) / MAP_SCALE, z0 / MAP_SCALE, (sy - uy) / MAP_SCALE);
        impl_glTexCoord2f(1, 1); impl_glVertex3f((sx + ux) / MAP_SCALE, z0 / MAP_SCALE, (sy + uy) / MAP_SCALE);
        impl_glEnd();
        if (s % 7 == 3) impl_glDisable(GL_BLEND);
    }
    set_fog(0);

    /* HUD：2D */
    if (g_hud) {
        impl_glMatrixMode(GL_PROJECTION);
        impl_glLoadIdentity();
        impl_glOrtho(0, 320, 200, 0, -1, 1);
        impl_glMatrixMode(GL_MODELVIEW);
        impl_glLoadIdentity();
        impl_glDisable(GL_DEPTH_TEST);
        impl_glColor4f(1, 1, 1, 1);
        for (int k = 0; k < 12; k++) {
            bind(hud_tex[k]);
            const float x0 = k * 26.0f + 4, x1 = x0 + (k == 0 ? 320 : 22), y0 = k == 0 ? 168 : 172, y1 = k == 0 ? 200 : 196;
            impl_glBegin(GL_TRIANGLE_STRIP);
            impl_glTexCoord2f(0, 0); impl_glVertex2f(k == 0 ? 0 : x0, y0);
            impl_glTexCoord2f(1, 0); impl_glVertex2f(k == 0 ? 320 : x1, y0);
            impl_glTexCoord2f(0, 1); impl_glVertex2f(k == 0 ? 0 : x0, y1);
            impl_glTexCoord2f(1, 1); impl_glVertex2f(k == 0 ? 320 : x1, y1);
            impl_glEnd();
        }
    }
    ewglSwapBuffers(hdc);
}

int count_cracks(int rows)
{
    int n = 0;
    for (int y = 0; y < std::min(rows, g_fake_fh); y++) {
        for (int x = 0; x < g_fake_fw; x++) {
            if (g_fake_frame[size_t(y) * g_fake_fw + x] == 0xF81F) n++;
        }
    }
    return n;
}

} // namespace

int main(int argc, char **argv)
{
    int frames = 8;
    const char *bmp = nullptr;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--lists") g_lists = true;
        else if (a == "--sortlight") g_sortlight = true;
        else if (a == "--jitter") g_jitter = true;
        else if (a == "--nofog") g_fog = false;
        else if (a == "--nohud") g_hud = false;
        else if (a == "--bmp" && i + 1 < argc) bmp = argv[++i];
        else if (a == "--frames" && i + 1 < argc) frames = atoi(argv[++i]);
        else if (a == "--n" && i + 1 < argc) N = atoi(argv[++i]);
        else if (a == "--sub" && i + 1 < argc) SUB = atoi(argv[++i]);
        else if (a == "--irr") g_irr = true;
        else if (a == "--norender") g_fake_render = false;
    }
    HDC hdc = reinterpret_cast<HDC>(0x1000);
    HGLRC rc = ewglCreateContext(hdc);
    ewglMakeCurrent(hdc, rc);
    build_map();
    impl_glGenTextures(24, wall_tex);
    impl_glGenTextures(8, flat_tex);
    impl_glGenTextures(6, spr_tex);
    impl_glGenTextures(12, hud_tex);
    for (int k = 0; k < 24; k++) make_tex(wall_tex[k], 64, 128, 100 + k, false);
    for (int k = 0; k < 8; k++) make_tex(flat_tex[k], 64, 64, 200 + k, false);
    for (int k = 0; k < 6; k++) make_tex(spr_tex[k], 32, 56, 300 + k, true);
    for (int k = 0; k < 12; k++) make_tex(hud_tex[k], k == 0 ? 320 : 16, k == 0 ? 32 : 24, 400 + k, false);
    collect_walls(g_walls);
    init_lists();

    uint64_t tot_wire = 0, tot_raw = 0;
    double host_us = 0;
    int counted = 0, cracks = 0;
    FakeStats acc;
    for (int f = 0; f < frames; f++) {
        fake_reset_stats();
        cam_yaw = 30.0f + f * 7.0f;
        cam_x = 20.0f + f * 3.0f;
        {
            const int ci = std::clamp(int((cam_x + N * 64) / 128), 0, N - 1), cj = std::clamp(int((cam_y + N * 64) / 128), 0, N - 1);
            cam_z = cells[size_t(cj) * N + ci].floor_h + 41.0f;
        }
        g_hud = true;
        const auto t0 = std::chrono::steady_clock::now();
        draw_frame(hdc);
        const auto t1 = std::chrono::steady_clock::now();
        if (f >= 2) host_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
        if (f >= 2) {
            tot_wire += g_fs.wire;
            tot_raw += g_fs.raw;
            counted++;
            for (int o = 0; o < 32; o++) {
                acc.op_bytes[o] += g_fs.op_bytes[o];
                acc.op_count[o] += g_fs.op_count[o];
            }
            acc.draw_verts += g_fs.draw_verts;
            acc.data_bytes += g_fs.data_bytes;
        }
        cracks += count_cracks(168);
    }
    printf("walls=%zu  frames=%d (steady avg over %d)\n", g_walls.size(), frames, counted);
    printf("per frame: wire %.0f B  raw %.0f B  (LZ %.2fx)  verts %.0f  data %.0f  split_draws %d\n",
           double(tot_wire) / counted, double(tot_raw) / counted, double(tot_raw) / std::max<uint64_t>(1, tot_wire),
           double(acc.draw_verts) / counted, double(acc.data_bytes) / counted, g_split_draws);
    for (int o = 0; o < 32; o++) {
        if (acc.op_count[o]) {
            printf("  %-10s %7.1f cmds  %8.0f B\n", fake_op_name(o), double(acc.op_count[o]) / counted,
                   double(acc.op_bytes[o]) / counted);
        }
    }
    printf("crack pixels (all frames, 3D area): %d\n", cracks);
    printf("host time per frame: %.0f us\n", host_us / counted);
    if (bmp) fake_write_bmp(bmp);
    ewglMakeCurrent(nullptr, nullptr);
    ewglDeleteContext(rc);
    return 0;
}
