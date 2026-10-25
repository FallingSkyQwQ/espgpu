/*
 * gl_misc.cpp — 清屏、同步、回读、光栅位置与像素绘制
 */

#include "gl_context.h"

#include <algorithm>
#include <cmath>

namespace espgl {

namespace {

constexpr GLuint kDrawPixelsTex = 0xFFFFFFF0u; /* glDrawPixels 用的私有纹理名 */

inline uint16_t to565(const float *c)
{
    auto q = [](float v, int m) { return static_cast<int>(std::lround((v < 0 ? 0 : v > 1 ? 1 : v) * m)); };
    return static_cast<uint16_t>((q(c[0], 31) << 11) | (q(c[1], 63) << 5) | q(c[2], 31));
}

} // namespace

void impl_glClear(GLbitfield mask)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glClear(mask));
    if (c->in_begin) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    if (g_sh.dev == nullptr) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    flush_pending(c);
    sync_viewport_scissor(c);
    uint8_t flags = 0;
    const State &s = c->st;
    if ((mask & GL_COLOR_BUFFER_BIT) && (s.color_mask[0] || s.color_mask[1] || s.color_mask[2])) {
        flags |= 1;
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && s.depth_mask) {
        flags |= 2;
    }
    if (flags == 0) {
        return;
    }
    const uint16_t depth = static_cast<uint16_t>(std::lround(s.clear_depth * 65535.0));
    espgpu_cmd_clear_ex(g_sh.dev, flags, to565(s.clear_color), depth);
}

void impl_glFlush(void)
{
    ESPGL_CTX(c);
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    flush_pending(c);
    if (g_sh.dev) {
        espgpu_cmd_flush(g_sh.dev, 200);
    }
}

void impl_glFinish(void)
{
    ESPGL_CTX(c);
    if (g_sh.dev == nullptr) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    flush_pending(c);
    const uint32_t v = ++g_sh.fence;
    espgpu_cmd_fence(g_sh.dev, v);
    espgpu_cmd_flush(g_sh.dev, 500);
    espgpu_wait_fence(g_sh.dev, v, 1000);
}

/* ── glReadPixels ─────────────────────────────────────────────────── */

void impl_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels)
{
    ESPGL_CTX(c);
    if (pixels == nullptr || w <= 0 || h <= 0) {
        return;
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
    if (format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX) {
        /* 深度 / 模板没法回读：给远平面 / 0 */
        if (format == GL_DEPTH_COMPONENT && type == GL_FLOAT) {
            auto *f = static_cast<float *>(pixels);
            std::fill(f, f + static_cast<size_t>(w) * h, 1.0f);
        } else {
            memset(pixels, format == GL_DEPTH_COMPONENT ? 0xFF : 0,
                   static_cast<size_t>(w) * h * (type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4));
        }
        return;
    }
    if (g_sh.dev) {
        /* 读回的是设备最近一帧：先把当前内容呈现出来 */
        std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
        flush_pending(c);
        espgpu_cmd_present(g_sh.dev, 1000);
        if (espgpu_wait_frame(g_sh.dev, g_sh.last_frame_id, 500) == 1) {
            g_sh.last_frame_id = espgpu_frame_id(g_sh.dev);
        }
        const size_t cap = static_cast<size_t>(g_sh.dev_w) * g_sh.dev_h * 4;
        std::vector<uint8_t> bgra(cap);
        int fw = 0, fh = 0;
        uint32_t fid = 0;
        if (espgpu_frame_snapshot_bgra(g_sh.dev, bgra.data(), cap, 0, &fw, &fh, &fid) == 0 && fw > 0 && fh > 0) {
            const double sx = static_cast<double>(fw) / std::max(1, c->win_w);
            const double sy = static_cast<double>(fh) / std::max(1, c->win_h);
            for (GLsizei j = 0; j < h; j++) {
                const int wy = y + j; /* GL 行，自下而上 */
                int dy = fh - 1 - static_cast<int>((wy + 0.5) * sy);
                dy = std::clamp(dy, 0, fh - 1);
                for (GLsizei i = 0; i < w; i++) {
                    int dx = static_cast<int>((x + i + 0.5) * sx);
                    dx = std::clamp(dx, 0, fw - 1);
                    const uint8_t *s = &bgra[(static_cast<size_t>(dy) * fw + dx) * 4];
                    uint8_t *o = &rgba[(static_cast<size_t>(j) * w + i) * 4];
                    o[0] = s[2];
                    o[1] = s[1];
                    o[2] = s[0];
                    o[3] = 255;
                }
            }
        }
    }
    tex_pack_from_rgba(c, w, h, format, type, rgba.data(), pixels);
}

/* ── 光栅位置 ─────────────────────────────────────────────────────── */

static void raster_pos(Context *c, float x, float y, float z, float w)
{
    const float in[4] = {x, y, z, w};
    float eye[4], clip[4];
    mat_xform(c->modelview.top(), in, eye);
    mat_xform(c->projection.top(), eye, clip);
    State &s = c->st;
    if (clip[3] <= 0.0f) {
        s.raster_valid = false;
        return;
    }
    const float nx = clip[0] / clip[3], ny = clip[1] / clip[3], nz = clip[2] / clip[3];
    s.raster_valid = std::fabs(nx) <= 1.0001f && std::fabs(ny) <= 1.0001f && std::fabs(nz) <= 1.0001f;
    s.raster_pos[0] = s.viewport[0] + (nx + 1.0f) * 0.5f * s.viewport[2];
    s.raster_pos[1] = s.viewport[1] + (ny + 1.0f) * 0.5f * s.viewport[3];
    s.raster_pos[2] = static_cast<float>(s.depth_range[0] + (nz + 1.0) * 0.5 * (s.depth_range[1] - s.depth_range[0]));
    s.raster_pos[3] = clip[3];
    memcpy(s.raster_color, s.cur_color, sizeof(s.raster_color));
}

#define RP2(fn, T)                                                                                 \
    void impl_##fn(T x, T y) { ESPGL_CTX(c); ESPGL_RECORD(c, impl_##fn(x, y)); raster_pos(c, (float)x, (float)y, 0, 1); } \
    void impl_##fn##v(const T *v) { if (v) impl_##fn(v[0], v[1]); }
#define RP3(fn, T)                                                                                 \
    void impl_##fn(T x, T y, T z) { ESPGL_CTX(c); ESPGL_RECORD(c, impl_##fn(x, y, z)); raster_pos(c, (float)x, (float)y, (float)z, 1); } \
    void impl_##fn##v(const T *v) { if (v) impl_##fn(v[0], v[1], v[2]); }
#define RP4(fn, T)                                                                                 \
    void impl_##fn(T x, T y, T z, T w) { ESPGL_CTX(c); ESPGL_RECORD(c, impl_##fn(x, y, z, w)); raster_pos(c, (float)x, (float)y, (float)z, (float)w); } \
    void impl_##fn##v(const T *v) { if (v) impl_##fn(v[0], v[1], v[2], v[3]); }

RP2(glRasterPos2d, GLdouble)
RP2(glRasterPos2f, GLfloat)
RP2(glRasterPos2i, GLint)
RP2(glRasterPos2s, GLshort)
RP3(glRasterPos3d, GLdouble)
RP3(glRasterPos3f, GLfloat)
RP3(glRasterPos3i, GLint)
RP3(glRasterPos3s, GLshort)
RP4(glRasterPos4d, GLdouble)
RP4(glRasterPos4f, GLfloat)
RP4(glRasterPos4i, GLint)
RP4(glRasterPos4s, GLshort)

void impl_glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove,
                   const GLubyte *bitmap)
{
    ESPGL_CTX(c);
    (void)width;
    (void)height;
    (void)xorig;
    (void)yorig;
    (void)bitmap; /* 位图本身不画（老式字体路径），只推进光栅位置 */
    c->st.raster_pos[0] += xmove;
    c->st.raster_pos[1] += ymove;
}

/**
 * glDrawPixels：上传成私有纹理，按光栅位置画一个屏幕对齐的四边形。
 * 临时改掉纹理 / 光照 / 矩阵，画完恢复（混合、alpha 测试、深度测试等仍按 GL 语义生效）。
 */
void impl_glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
    ESPGL_CTX(c);
    if (width <= 0 || height <= 0 || pixels == nullptr || !c->st.raster_valid || g_sh.dev == nullptr) {
        return;
    }
    if (format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX) {
        return;
    }
    flush_pending(c);
    Texture *t;
    {
        std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
        t = tex_lookup(kDrawPixelsTex, true);
        tex_unpack_to_rgba(c, width, height, format, type, pixels, t->rgba, nullptr);
        t->w = width;
        t->h = height;
        t->internal_format = GL_RGBA;
        t->paletted = false;
        t->has_palette = false;
        t->palette_format = GL_RGBA;
        t->has_image = true;
        t->device_only = false;
        t->dirty = true;
        t->upload = true;
        t->min_filter = t->mag_filter = GL_NEAREST;
        t->wrap_s = t->wrap_t = GL_CLAMP_TO_EDGE;
    }

    const State saved = c->st;
    const Mat4 mv = c->modelview.top(), pj = c->projection.top();
    const Mat4 tm = c->texmat[0].top();

    State &s = c->st;
    s.lighting = false;
    s.fog = s.fog; /* 雾按 GL 语义仍生效 */
    s.unit[0].enable_2d = true;
    s.unit[0].enable_1d = false;
    s.unit[0].bound_2d = kDrawPixelsTex;
    s.unit[0].env_mode = GL_REPLACE;
    s.unit[1].enable_2d = s.unit[1].enable_1d = false;
    s.cull = false;
    c->modelview.top() = Mat4::identity();
    c->projection.top() = Mat4::identity();
    c->texmat[0].top() = Mat4::identity();
    c->key_dirty = true;

    /* 窗口坐标 → NDC（以当前视口为准，和 GL 的定义一致） */
    const float vx = static_cast<float>(s.viewport[0]), vy = static_cast<float>(s.viewport[1]);
    const float vw = static_cast<float>(std::max(1, s.viewport[2])), vh = static_cast<float>(std::max(1, s.viewport[3]));
    const float x0 = s.raster_pos[0], y0 = s.raster_pos[1];
    const float x1 = x0 + width * s.zoom_x, y1 = y0 + height * s.zoom_y;
    auto ndc_x = [&](float wx) { return (wx - vx) / vw * 2.0f - 1.0f; };
    auto ndc_y = [&](float wy) { return (wy - vy) / vh * 2.0f - 1.0f; };
    const float z = static_cast<float>(saved.raster_pos[2] * 2.0 - 1.0);

    Vertex q[4];
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const float px[4] = {x0, x1, x1, x0};
    const float py[4] = {y0, y0, y1, y1};
    for (int k = 0; k < 4; k++) {
        current_vertex(c, q[k]);
        q[k].pos[0] = ndc_x(px[k]);
        q[k].pos[1] = ndc_y(py[k]);
        q[k].pos[2] = z;
        q[k].pos[3] = 1;
        q[k].tc[0][0] = uv[k][0];
        q[k].tc[0][1] = uv[k][1];
        memcpy(q[k].color, saved.raster_color, sizeof(q[k].color));
    }
    emit(c, GL_QUADS, q, 4);
    flush_pending(c);

    c->st = saved;
    c->modelview.top() = mv;
    c->projection.top() = pj;
    c->texmat[0].top() = tm;
    c->key_dirty = true;
}

void impl_glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)type;
    gl_unimplemented(FN_glCopyPixels);
}

} // namespace espgl
