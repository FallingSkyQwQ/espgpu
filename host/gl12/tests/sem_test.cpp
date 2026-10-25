/*
 * sem_test.cpp — host/gl12 的 GL 语义测试：经真实 firmware/core 渲染后检查像素 / 状态 / 命令统计。
 * 窗口 640x400（shim），设备 320x200。
 */
#include "gl_context.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
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
extern bool g_fake_connected;
extern int g_fake_opens;
void fake_reset_stats();
void fake_write_bmp(const char *path);

static int fails = 0;
#define CHECK(c, ...)                                                                                              \
    do {                                                                                                           \
        if (!(c)) {                                                                                                \
            fails++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static HDC g_hdc = reinterpret_cast<HDC>(0x1000);

static void rgb_at(int x, int y, int out[3])
{
    const uint16_t v = g_fake_frame[size_t(y) * g_fake_fw + x];
    out[0] = ((v >> 11) & 31) * 255 / 31;
    out[1] = ((v >> 5) & 63) * 255 / 63;
    out[2] = (v & 31) * 255 / 31;
}
static bool near_rgb(int x, int y, int r, int g, int b, int tol = 24)
{
    int c[3];
    rgb_at(x, y, c);
    return std::abs(c[0] - r) <= tol && std::abs(c[1] - g) <= tol && std::abs(c[2] - b) <= tol;
}
#define PIX(x, y, r, g, b, what)                                                                                  \
    do {                                                                                                           \
        int c_[3];                                                                                                 \
        rgb_at(x, y, c_);                                                                                          \
        CHECK(near_rgb(x, y, r, g, b), "%s: pixel (%d,%d) = %d,%d,%d want %d,%d,%d", what, x, y, c_[0], c_[1],    \
              c_[2], r, g, b);                                                                                     \
    } while (0)

/* 2D：窗口像素坐标（左上原点）画到 640x400 窗口 = 设备 320x200（设备坐标 = 窗口/2） */
static void ortho2d()
{
    impl_glViewport(0, 0, 640, 400);
    impl_glMatrixMode(GL_PROJECTION);
    impl_glLoadIdentity();
    impl_glOrtho(0, 640, 400, 0, -1, 1);
    impl_glMatrixMode(GL_MODELVIEW);
    impl_glLoadIdentity();
    impl_glDisable(GL_DEPTH_TEST);
    impl_glDisable(GL_TEXTURE_2D);
    impl_glDisable(GL_BLEND);
    impl_glDisable(GL_ALPHA_TEST);
    impl_glDisable(GL_FOG);
    impl_glDisable(GL_SCISSOR_TEST);
    impl_glShadeModel(GL_SMOOTH);
}
static void clear(float r, float g, float b)
{
    impl_glClearColor(r, g, b, 1);
    impl_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}
static void frame() { ewglSwapBuffers(g_hdc); }

static void quad(float x0, float y0, float x1, float y1)
{
    impl_glBegin(GL_QUADS);
    impl_glTexCoord2f(0, 0); impl_glVertex2f(x0, y0);
    impl_glTexCoord2f(1, 0); impl_glVertex2f(x1, y0);
    impl_glTexCoord2f(1, 1); impl_glVertex2f(x1, y1);
    impl_glTexCoord2f(0, 1); impl_glVertex2f(x0, y1);
    impl_glEnd();
}

static void test_flat_provoking()
{
    ortho2d();
    clear(0, 0, 0);
    impl_glShadeModel(GL_FLAT);
    /* GL_QUADS：激发顶点 = 第 4 个 */
    impl_glBegin(GL_QUADS);
    impl_glColor3f(1, 0, 0); impl_glVertex2f(0, 0);
    impl_glColor3f(0, 1, 0); impl_glVertex2f(100, 0);
    impl_glColor3f(0, 0, 1); impl_glVertex2f(100, 100);
    impl_glColor3f(1, 1, 0); impl_glVertex2f(0, 100);
    impl_glEnd();
    /* 4 顶点 strip，激发顶点各不同：三角形 (0,1,2) 取 v2，(1,3,2) 取 v3 */
    impl_glBegin(GL_TRIANGLE_STRIP);
    impl_glColor3f(1, 0, 0); impl_glVertex2f(200, 0);
    impl_glColor3f(0, 1, 0); impl_glVertex2f(300, 0);
    impl_glColor3f(0, 0, 1); impl_glVertex2f(200, 100);
    impl_glColor3f(1, 1, 1); impl_glVertex2f(300, 100);
    impl_glEnd();
    /* GL_POLYGON：激发顶点 = 第 1 个 */
    impl_glBegin(GL_POLYGON);
    impl_glColor3f(1, 0, 1); impl_glVertex2f(400, 0);
    impl_glColor3f(0, 1, 0); impl_glVertex2f(500, 0);
    impl_glColor3f(0, 0, 1); impl_glVertex2f(520, 50);
    impl_glColor3f(1, 1, 0); impl_glVertex2f(500, 100);
    impl_glColor3f(0, 1, 1); impl_glVertex2f(400, 100);
    impl_glEnd();
    /* GL_QUAD_STRIP：第 i 个四边形取 2i+3（0 起） */
    impl_glBegin(GL_QUAD_STRIP);
    impl_glColor3f(1, 0, 0); impl_glVertex2f(0, 200);
    impl_glColor3f(0, 1, 0); impl_glVertex2f(0, 300);
    impl_glColor3f(0, 0, 1); impl_glVertex2f(100, 200);
    impl_glColor3f(1, 1, 1); impl_glVertex2f(100, 300);
    impl_glEnd();
    /* 4 顶点 fan，激发顶点 v2 / v3 不同 */
    impl_glBegin(GL_TRIANGLE_FAN);
    impl_glColor3f(1, 0, 0); impl_glVertex2f(200, 200);
    impl_glColor3f(0, 1, 0); impl_glVertex2f(300, 200);
    impl_glColor3f(0, 0, 1); impl_glVertex2f(300, 300);
    impl_glColor3f(1, 1, 0); impl_glVertex2f(200, 300);
    impl_glEnd();
    frame();
    PIX(10, 25, 255, 255, 0, "QUADS flat = v3");
    PIX(40, 10, 255, 255, 0, "QUADS flat = v3 (2nd tri)");
    PIX(105, 10, 0, 0, 255, "STRIP tri0 = v2");  /* 设备 (105,10) = 窗口 (210,20)：靠近左上，三角形 0 */
    PIX(145, 40, 255, 255, 255, "STRIP tri1 = v3"); /* 窗口 (290,80) */
    PIX(225, 25, 255, 0, 255, "POLYGON flat = v0");
    PIX(25, 125, 255, 255, 255, "QUAD_STRIP flat = v3");
    PIX(145, 105, 0, 0, 255, "FAN tri0 = v2"); /* 窗口 (290,210)：右上，三角形 (0,1,2) */
    PIX(105, 145, 255, 255, 0, "FAN tri1 = v3"); /* 窗口 (210,290) */
    impl_glShadeModel(GL_SMOOTH);
}

static void test_strip_diagonal()
{
    ortho2d();
    clear(1, 0, 1);
    /* v1、v2 黑，v0 红，v3 绿：GL 的 strip 对角线是 v1-v2，正方形中心是黑 */
    impl_glBegin(GL_TRIANGLE_STRIP);
    impl_glColor3f(1, 0, 0); impl_glVertex2f(0, 0);
    impl_glColor3f(0, 0, 0); impl_glVertex2f(200, 0);
    impl_glColor3f(0, 0, 0); impl_glVertex2f(0, 200);
    impl_glColor3f(0, 1, 0); impl_glVertex2f(200, 200);
    impl_glEnd();
    frame();
    PIX(50, 50, 0, 0, 0, "strip quad diagonal v1-v2");
}

static GLuint make_tex(int w, int h, GLenum ifmt, GLenum fmt, GLenum type, const void *px)
{
    GLuint t;
    impl_glGenTextures(1, &t);
    impl_glBindTexture(GL_TEXTURE_2D, t);
    impl_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    impl_glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, px);
    return t;
}

static void test_tex_formats()
{
    /* RGB 3x2，UNPACK_ALIGNMENT 4：每行 9 字节补到 12 */
    const uint8_t rgb[24] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0,
                             255, 255, 0, 0, 255, 255, 255, 0, 255, 0, 0, 0};
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    GLuint t_rgb = make_tex(3, 2, GL_RGB, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    /* LUMINANCE 3x2，对齐 1 */
    const uint8_t lum[6] = {0, 128, 255, 255, 128, 0};
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    GLuint t_lum = make_tex(3, 2, GL_LUMINANCE, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
    /* BGRA + ROW_LENGTH / SKIP：从 4x3 的大图里取 (1,1) 起的 2x2 */
    uint8_t big[4 * 3 * 4] = {};
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t *p = &big[(y * 4 + x) * 4];
        p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
    };
    put(1, 1, 255, 0, 0); put(2, 1, 0, 255, 0); put(1, 2, 0, 0, 255); put(2, 2, 255, 255, 255);
    impl_glPixelStorei(GL_UNPACK_ROW_LENGTH, 4);
    impl_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    impl_glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    GLuint t_bgra = make_tex(2, 2, GL_RGBA, 0x80E1 /*GL_BGRA*/, GL_UNSIGNED_BYTE, big);
    impl_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    impl_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    impl_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    /* float 数据，对齐 8 但分量 4 字节：GL 规定不补齐 */
    const float fl[2 * 2 * 3] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0};
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    GLuint t_f = make_tex(2, 2, GL_RGB, GL_RGB, GL_FLOAT, fl);
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    /* 默认 MIN_FILTER = NEAREST_MIPMAP_LINEAR、没有 mipmap：仍按层级 0 采样 */
    const uint8_t one[4] = {0, 0, 255, 255};
    GLuint t_def;
    impl_glGenTextures(1, &t_def);
    impl_glBindTexture(GL_TEXTURE_2D, t_def);
    impl_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, one);

    ortho2d();
    clear(0, 0, 0);
    impl_glEnable(GL_TEXTURE_2D);
    impl_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    impl_glBindTexture(GL_TEXTURE_2D, t_rgb); quad(0, 0, 300, 200);
    impl_glBindTexture(GL_TEXTURE_2D, t_lum); quad(320, 0, 620, 200);
    impl_glBindTexture(GL_TEXTURE_2D, t_bgra); quad(0, 200, 200, 400);
    impl_glBindTexture(GL_TEXTURE_2D, t_f); quad(220, 200, 420, 400);
    impl_glBindTexture(GL_TEXTURE_2D, t_def); quad(440, 200, 640, 400);
    frame();
    /* t=0 是第一行，画在屏幕上方 */
    PIX(25, 25, 255, 0, 0, "RGB align4 (0,0)");
    PIX(75, 25, 0, 255, 0, "RGB align4 (1,0)");
    PIX(125, 25, 0, 0, 255, "RGB align4 (2,0)");
    PIX(25, 75, 255, 255, 0, "RGB align4 (0,1)");
    PIX(125, 75, 255, 0, 255, "RGB align4 (2,1)");
    PIX(185, 25, 0, 0, 0, "LUM (0,0)");
    PIX(285, 25, 255, 255, 255, "LUM (2,0)");
    PIX(185, 75, 255, 255, 255, "LUM (0,1)");
    PIX(25, 125, 255, 0, 0, "BGRA skip (0,0)");
    PIX(75, 125, 0, 255, 0, "BGRA skip (1,0)");
    PIX(25, 175, 0, 0, 255, "BGRA skip (0,1)");
    PIX(75, 175, 255, 255, 255, "BGRA skip (1,1)");
    PIX(130, 125, 255, 0, 0, "float align8 (0,0)");
    PIX(180, 125, 0, 255, 0, "float align8 (1,0)");
    PIX(130, 175, 0, 0, 255, "float align8 (0,1)");
    PIX(180, 175, 255, 255, 0, "float align8 (1,1)");
    PIX(270, 150, 0, 0, 255, "default min filter samples level 0");
    impl_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    impl_glDisable(GL_TEXTURE_2D);
}

static void test_subimage_after_use()
{
    const uint8_t red[4 * 4] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    const uint8_t green[4 * 4] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    GLuint t = make_tex(2, 2, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, red);
    ortho2d();
    clear(0, 0, 0);
    impl_glEnable(GL_TEXTURE_2D);
    impl_glBindTexture(GL_TEXTURE_2D, t);
    impl_glColor3f(1, 1, 1);
    quad(0, 0, 100, 100);
    impl_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, green);
    quad(200, 0, 300, 100);
    /* 参数变化同理：先画的用旧过滤 / 环绕 */
    impl_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    quad(400, 0, 500, 100);
    frame();
    PIX(25, 25, 255, 0, 0, "quad drawn before glTexSubImage keeps old texels");
    PIX(125, 25, 0, 255, 0, "quad drawn after glTexSubImage");
    PIX(225, 25, 0, 255, 0, "after param change");
    impl_glDisable(GL_TEXTURE_2D);
}

static void test_reorder_blend()
{
    /* 透视场景：不透明 A（近，红），半透明 B（中，蓝，50%），不透明 C（远，绿）——三者重叠 */
    impl_glViewport(0, 0, 640, 400);
    impl_glMatrixMode(GL_PROJECTION);
    impl_glLoadIdentity();
    impl_glFrustum(-1, 1, -1, 1, 1, 100);
    impl_glMatrixMode(GL_MODELVIEW);
    impl_glLoadIdentity();
    impl_glEnable(GL_DEPTH_TEST);
    impl_glDepthFunc(GL_LEQUAL);
    impl_glDepthMask(GL_TRUE);
    impl_glDisable(GL_TEXTURE_2D);
    clear(0, 0, 0);
    auto sq = [](float x0, float x1, float z) {
        impl_glBegin(GL_QUADS);
        impl_glVertex3f(x0, -1, z); impl_glVertex3f(x1, -1, z); impl_glVertex3f(x1, 1, z); impl_glVertex3f(x0, 1, z);
        impl_glEnd();
    };
    impl_glColor3f(1, 0, 0);
    sq(-2, -0.5f, -2); /* A：左边 */
    impl_glEnable(GL_BLEND);
    impl_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    impl_glDepthMask(GL_FALSE);
    impl_glColor4f(0, 0, 1, 0.5f);
    sq(-3, 3, -4); /* B：整块 */
    impl_glDisable(GL_BLEND);
    impl_glDepthMask(GL_TRUE);
    impl_glColor3f(0, 1, 0);
    sq(-6, 6, -8); /* C：最远（GL 次序：B 先混到黑底上，C 在 B 后面但 B 没写深度 → C 覆盖 B） */
    impl_glColor3f(1, 0, 0);
    sq(-2, -0.5f, -2); /* 再画一次 A：和第一次 A 状态相同（可合并），结果不变 */
    frame();
    PIX(40, 100, 255, 0, 0, "A in front");
    PIX(250, 100, 0, 255, 0, "C drawn after B (B wrote no depth) covers it");
    impl_glDisable(GL_DEPTH_TEST);
}

static void test_lazy_state()
{
    ortho2d();
    clear(0, 0, 0);
    impl_glColor3f(1, 1, 1);
    frame();
    fake_reset_stats();
    clear(0, 0, 0);
    impl_glColor3f(1, 1, 1);
    quad(0, 0, 10, 10);
    impl_glBlendFunc(GL_ONE, GL_ONE); /* 混合关着：不影响设备状态 */
    impl_glAlphaFunc(GL_GREATER, 0.3f);
    impl_glFogf(GL_FOG_DENSITY, 0.7f);
    impl_glDepthFunc(GL_ALWAYS);
    impl_glPolygonOffset(1, 1);
    impl_glPushMatrix();
    impl_glTranslatef(20, 0, 0); /* 只是平移：折进同一批次 */
    quad(0, 0, 10, 10);
    impl_glPopMatrix();
    frame();
    CHECK(g_fs.op_count[6] == 1, "irrelevant state changes split the batch: %llu draws",
          (unsigned long long)g_fs.op_count[6]);
    CHECK(g_fs.op_count[2] <= 1, "matrix push/translate/pop resent MVP %llu times", (unsigned long long)g_fs.op_count[2]);
    PIX(12, 2, 255, 255, 255, "folded quad position");
    PIX(16, 2, 0, 0, 0, "folded quad right edge");
    PIX(5, 2, 0, 0, 0, "gap between quads");
    impl_glBlendFunc(GL_ONE, GL_ZERO);
    impl_glAlphaFunc(GL_ALWAYS, 0);
    impl_glDepthFunc(GL_LESS);
    impl_glPolygonOffset(0, 0);
}

static void test_lists()
{
    ortho2d();
    GLuint base = impl_glGenLists(3);
    impl_glNewList(base, GL_COMPILE);
    impl_glColor3f(0, 1, 0);
    for (int i = 0; i < 12; i++) {
        impl_glBegin(GL_TRIANGLE_STRIP);
        impl_glVertex2f(i * 50.0f, 0); impl_glVertex2f(i * 50.0f + 40, 0);
        impl_glVertex2f(i * 50.0f, 40); impl_glVertex2f(i * 50.0f + 40, 40);
        impl_glEnd();
    }
    impl_glEndList();
    /* 列表里改基址再 CallLists：基址按执行时取 */
    impl_glNewList(base + 1, GL_COMPILE);
    impl_glListBase(base);
    GLubyte ids[1] = {0};
    impl_glCallLists(1, GL_UNSIGNED_BYTE, ids);
    impl_glEndList();
    for (int f = 0; f < 3; f++) {
        if (f == 2) {
            fake_reset_stats();
        }
        clear(0, 0, 0);
        impl_glListBase(1000);
        impl_glCallList(base + 1);
        impl_glListBase(0);
        frame();
        PIX(10, 10, 0, 255, 0, "list strip 0");
        PIX(260, 10, 0, 255, 0, "list strip 10");
        PIX(22, 10, 0, 0, 0, "gap");
    }
    CHECK(g_fs.data_bytes == 0, "list buffer re-uploaded on 3rd call (%llu B)", (unsigned long long)g_fs.data_bytes);
    impl_glDeleteLists(base, 3);
    CHECK(!impl_glIsList(base), "deleted");
}

static void test_readpixels()
{
    ortho2d();
    clear(0, 0, 1);
    impl_glColor3f(1, 0, 0);
    quad(0, 0, 640, 200); /* 上半红 */
    impl_glFlush();
    uint8_t px[3 * 3 * 2] = {};
    impl_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    impl_glReadPixels(0, 0, 3, 2, GL_RGB, GL_UNSIGNED_BYTE, px); /* 窗口左下角：蓝 */
    CHECK(px[2] > 200 && px[0] < 40, "readpixels bottom-left should be blue: %u %u %u", px[0], px[1], px[2]);
    uint8_t top[3] = {};
    impl_glReadPixels(0, 399, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, top);
    CHECK(top[0] > 200 && top[2] < 40, "readpixels top row should be red: %u %u %u", top[0], top[1], top[2]);
    impl_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    frame();
}

static void test_scissor_clear()
{
    ortho2d();
    clear(0, 0, 0);
    impl_glEnable(GL_SCISSOR_TEST);
    impl_glScissor(0, 0, 320, 200); /* 窗口左下四分之一 */
    impl_glClearColor(0, 1, 0, 1);
    impl_glClear(GL_COLOR_BUFFER_BIT);
    impl_glDisable(GL_SCISSOR_TEST);
    frame();
    PIX(10, 190, 0, 255, 0, "scissored clear bottom-left");
    PIX(10, 10, 0, 0, 0, "outside scissor top-left");
    PIX(300, 190, 0, 0, 0, "outside scissor bottom-right");
}

static void test_viewport_minimized()
{
    ortho2d();
    clear(0, 0, 0);
    frame();
    shim_client_w() = 0;
    shim_client_h() = 0;
    frame(); /* 最小化时 SwapBuffers */
    clear(0, 0, 0);
    impl_glColor3f(1, 1, 1);
    quad(0, 0, 640, 400);
    frame();
    shim_client_w() = 640;
    shim_client_h() = 400;
    PIX(300, 190, 255, 255, 255, "after minimize the viewport still maps the whole window");
    CHECK(cur()->win_w == 640, "win_w %d", cur()->win_w);
}

static void test_matrices()
{
    impl_glMatrixMode(GL_MODELVIEW);
    impl_glLoadIdentity();
    impl_glRotatef(90, 0, 0, 1);
    impl_glTranslatef(1, 2, 3);
    impl_glScalef(2, 2, 2);
    double m[16];
    impl_glGetDoublev(GL_MODELVIEW_MATRIX, m);
    /* R(90,z) * T(1,2,3) * S(2)：x' = -2y - 2, y' = 2x + 1, z' = 2z + 3 */
    const double want[16] = {0, 2, 0, 0, -2, 0, 0, 0, 0, 0, 2, 0, -2, 1, 3, 1};
    for (int i = 0; i < 16; i++) {
        CHECK(std::fabs(m[i] - want[i]) < 1e-9, "modelview[%d] = %g want %g", i, m[i], want[i]);
    }
    impl_glMatrixMode(GL_PROJECTION);
    impl_glLoadIdentity();
    impl_glFrustum(-1, 2, -3, 4, 1, 10);
    impl_glGetDoublev(GL_PROJECTION_MATRIX, m);
    CHECK(std::fabs(m[0] - 2.0 / 3) < 1e-9 && std::fabs(m[8] - 1.0 / 3) < 1e-9 && std::fabs(m[11] + 1) < 1e-12 &&
              std::fabs(m[14] + 20.0 / 9) < 1e-9 && std::fabs(m[10] + 11.0 / 9) < 1e-9,
          "frustum");
    impl_glGetError();
    for (int i = 0; i < 4; i++) impl_glPushMatrix(); /* 投影栈深 4：第 4 次溢出 */
    CHECK(impl_glGetError() == GL_STACK_OVERFLOW, "projection stack overflow");
    for (int i = 0; i < 4; i++) impl_glPopMatrix();
    CHECK(impl_glGetError() == GL_STACK_UNDERFLOW, "projection stack underflow");
    impl_glLoadIdentity();
    impl_glMatrixMode(GL_MODELVIEW);
    impl_glLoadIdentity();
}

static void test_errors()
{
    impl_glGetError();
    impl_glBegin(0x20);
    CHECK(impl_glGetError() == GL_INVALID_ENUM, "glBegin bad mode");
    impl_glEnd();
    CHECK(impl_glGetError() == GL_INVALID_OPERATION, "glEnd without begin");
    impl_glViewport(0, 0, -1, 1);
    impl_glEnd();
    CHECK(impl_glGetError() == GL_INVALID_VALUE, "first error is sticky");
    CHECK(impl_glGetError() == GL_NO_ERROR, "error cleared");
    impl_glDrawArrays(GL_TRIANGLES, 0, -1);
    CHECK(impl_glGetError() == GL_INVALID_VALUE, "DrawArrays count < 0");
}

static void test_big_batch_no_deindex()
{
    ortho2d();
    clear(0, 0, 0);
    fake_reset_stats();
    impl_glBegin(GL_TRIANGLES);
    for (int i = 0; i < 2000; i++) {
        const float x = float(i % 64) * 10, y = float(i / 64) * 12;
        impl_glColor4f(float(i % 7) / 7, float(i % 5) / 5, 1, float(i % 3) / 3); /* 逐顶点 RGBA8 → 步长大 */
        impl_glVertex2f(x + 0.3f, y);
        impl_glVertex2f(x + 8.3f, y);
        impl_glVertex2f(x + 0.3f, y + 9);
    }
    impl_glEnd();
    frame();
    CHECK(g_split_draws == 0, "espgpu had to de-index %d draws", g_split_draws);
}

static void test_wgl_threads()
{
    HGLRC rc2 = ewglCreateContext(g_hdc);
    HGLRC mine = reinterpret_cast<HGLRC>(cur());
    std::atomic<int> r1{-1}, r2{-1};
    std::thread t([&] {
        r1 = ewglMakeCurrent(g_hdc, mine) ? 1 : 0; /* 已是主线程的当前上下文 */
        r2 = ewglDeleteContext(mine) ? 1 : 0;
    });
    t.join();
    CHECK(r1 == 0, "MakeCurrent of a context current on another thread must fail");
    CHECK(r2 == 0, "DeleteContext of a context current on another thread must fail");
    std::atomic<int> r3{-1};
    std::thread t2([&] {
        r3 = ewglMakeCurrent(g_hdc, rc2) ? 1 : 0;
        ewglMakeCurrent(nullptr, nullptr);
    });
    t2.join();
    CHECK(r3 == 1, "other thread can use another context");
    CHECK(ewglDeleteContext(rc2), "delete released context");
}

static void test_reconnect()
{
    const uint8_t red[4] = {255, 0, 0, 255};
    GLuint t = make_tex(1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, red);
    ortho2d();
    impl_glEnable(GL_TEXTURE_2D);
    impl_glBindTexture(GL_TEXTURE_2D, t);
    impl_glColor3f(1, 1, 1);
    clear(0, 0, 0);
    quad(0, 0, 100, 100);
    frame();
    PIX(10, 10, 255, 0, 0, "before disconnect");
    const int opens = g_fake_opens;
    g_fake_connected = false;
    clear(0, 0, 0);
    quad(0, 0, 100, 100);
    frame(); /* 掉线：丢帧并尝试重开 */
    CHECK(g_fake_opens > opens, "device reopened after disconnect");
    clear(0, 0, 0);
    quad(0, 0, 100, 100);
    frame();
    PIX(10, 10, 255, 0, 0, "texture re-uploaded after reconnect");
    impl_glDisable(GL_TEXTURE_2D);
}

static void test_paletted_and_reupload()
{
    /* Doom 的 gl_use_paletted_texture：COLOR_INDEX8 + glColorTableEXT，透明色 alpha = 0 配 alpha 测试 */
    const uint8_t pal[4 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 0, 0};
    const uint8_t idx[4] = {0, 1, 2, 3};
    GLuint t;
    impl_glGenTextures(1, &t);
    impl_glBindTexture(GL_TEXTURE_2D, t);
    impl_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    impl_glColorTable(GL_TEXTURE_2D, GL_RGBA, 4, GL_RGBA, GL_UNSIGNED_BYTE, pal);
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    impl_glTexImage2D(GL_TEXTURE_2D, 0, 0x80E5 /*GL_COLOR_INDEX8_EXT*/, 2, 2, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, idx);
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    ortho2d();
    clear(1, 1, 1);
    impl_glEnable(GL_TEXTURE_2D);
    impl_glEnable(GL_ALPHA_TEST);
    impl_glAlphaFunc(GL_GEQUAL, 0.5f);
    impl_glColor3f(1, 1, 1);
    quad(0, 0, 200, 200);
    frame();
    PIX(25, 25, 255, 0, 0, "paletted index 0");
    PIX(75, 25, 0, 255, 0, "paletted index 1");
    PIX(25, 75, 0, 0, 255, "paletted index 2");
    PIX(75, 75, 255, 255, 255, "paletted index 3 transparent (alpha test)");
    /* 再设一遍相同的调色板 / 相同的图像：不应重传 */
    fake_reset_stats();
    impl_glColorTable(GL_TEXTURE_2D, GL_RGBA, 4, GL_RGBA, GL_UNSIGNED_BYTE, pal);
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    impl_glTexImage2D(GL_TEXTURE_2D, 0, 0x80E5, 2, 2, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, idx);
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    clear(1, 1, 1);
    quad(0, 0, 200, 200);
    frame();
    CHECK(g_fs.op_count[5] == 0 && g_fs.op_count[17] == 0, "identical re-specification re-uploaded (%llu uploads, %llu palettes)",
          (unsigned long long)g_fs.op_count[5], (unsigned long long)g_fs.op_count[17]);
    PIX(25, 25, 255, 0, 0, "still correct");
    /* 改调色板：必须重传，且之前画的那块用旧颜色 */
    const uint8_t pal2[4 * 4] = {255, 255, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 0, 0};
    clear(1, 1, 1);
    quad(0, 0, 200, 200);
    impl_glColorTable(GL_TEXTURE_2D, GL_RGBA, 4, GL_RGBA, GL_UNSIGNED_BYTE, pal2);
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    impl_glTexImage2D(GL_TEXTURE_2D, 0, 0x80E5, 2, 2, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, idx);
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    quad(400, 0, 600, 200);
    frame();
    PIX(25, 25, 255, 0, 0, "drawn before palette change");
    PIX(225, 25, 255, 255, 0, "drawn after palette change");
    impl_glDisable(GL_ALPHA_TEST);
    impl_glDisable(GL_TEXTURE_2D);
}

static void test_vertex_arrays()
{
    ortho2d();
    clear(0, 0, 0);
    const float va[5 * 2] = {0, 0, 100, 0, 100, 100, 0, 100, 300, 300};
    const uint8_t ca[5 * 4] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 0, 255};
    const GLushort ix[6] = {0, 1, 2, 0, 2, 3};
    impl_glEnableClientState(GL_VERTEX_ARRAY);
    impl_glEnableClientState(GL_COLOR_ARRAY);
    impl_glVertexPointer(2, GL_FLOAT, 0, va);
    impl_glColorPointer(4, GL_UNSIGNED_BYTE, 0, ca);
    impl_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, ix);
    /* glArrayElement 在 begin/end 里 */
    impl_glBegin(GL_TRIANGLES);
    impl_glArrayElement(1);
    impl_glArrayElement(4);
    impl_glArrayElement(2);
    impl_glEnd();
    impl_glDisableClientState(GL_COLOR_ARRAY);
    impl_glDisableClientState(GL_VERTEX_ARRAY);
    frame();
    PIX(25, 25, 255, 0, 0, "DrawElements quad");
    PIX(75, 50, 191, 0, 0, "ArrayElement triangle (smooth red->black)");
}

static void test_drawpixels_lines()
{
    ortho2d();
    clear(0, 0, 0);
    impl_glColor3f(0, 0, 1);
    quad(400, 0, 500, 100); /* 之前的几何（同批次状态之外） */
    uint8_t img[4 * 4 * 3];
    for (int i = 0; i < 16; i++) {
        img[i * 3] = 0;
        img[i * 3 + 1] = 255;
        img[i * 3 + 2] = 0;
    }
    impl_glRasterPos2i(100, 300); /* 窗口坐标经 ortho：左上原点的 (100,300) → GL 窗口 y = 100 */
    impl_glPixelZoom(10, 10);
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    impl_glDrawPixels(4, 4, GL_RGB, GL_UNSIGNED_BYTE, img);
    impl_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    impl_glPixelZoom(1, 1);
    impl_glColor3f(1, 1, 0);
    impl_glBegin(GL_LINES);
    impl_glVertex2f(0, 381);
    impl_glVertex2f(640, 381);
    impl_glEnd();
    impl_glColor3f(0, 0, 1);
    quad(400, 200, 500, 300); /* 之后的几何：状态恢复了 */
    frame();
    PIX(60, 140, 0, 255, 0, "glDrawPixels at raster position (zoomed)");
    PIX(45, 140, 0, 0, 0, "left of glDrawPixels");
    PIX(160, 190, 255, 255, 0, "GL_LINES");
    PIX(225, 25, 0, 0, 255, "geometry before glDrawPixels");
    PIX(225, 125, 0, 0, 255, "geometry after glDrawPixels uses restored state");
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    HGLRC rc = ewglCreateContext(g_hdc);
    ewglMakeCurrent(g_hdc, rc);
    test_flat_provoking();
    test_strip_diagonal();
    test_tex_formats();
    test_subimage_after_use();
    test_reorder_blend();
    test_lazy_state();
    test_lists();
    test_readpixels();
    test_scissor_clear();
    test_viewport_minimized();
    test_matrices();
    test_errors();
    test_big_batch_no_deindex();
    test_wgl_threads();
    test_reconnect();
    test_paletted_and_reupload();
    test_vertex_arrays();
    test_drawpixels_lines();
    ewglMakeCurrent(nullptr, nullptr);
    ewglDeleteContext(rc);
    printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
