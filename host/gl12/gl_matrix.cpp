/*
 * gl_matrix.cpp — 矩阵栈（列主序，与 GL 一致；发给设备时再转行主序）
 */

#include "gl_context.h"

#include <cmath>

namespace espgl {

Mat4 mat_mul(const Mat4 &a, const Mat4 &b)
{
    Mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            double s = 0;
            for (int k = 0; k < 4; k++) {
                s += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = s;
        }
    }
    return r;
}

void mat_xform(const Mat4 &a, const float in[4], float out[4])
{
    for (int row = 0; row < 4; row++) {
        double s = 0;
        for (int k = 0; k < 4; k++) {
            s += a.m[k * 4 + row] * in[k];
        }
        out[row] = static_cast<float>(s);
    }
}

bool mat_inverse(const Mat4 &a, Mat4 &out)
{
    const double *m = a.m;
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0) {
        return false;
    }
    det = 1.0 / det;
    for (int i = 0; i < 16; i++) {
        out.m[i] = inv[i] * det;
    }
    return true;
}

Mat4 &cur_matrix(Context *c)
{
    switch (c->st.matrix_mode) {
    case GL_PROJECTION: return c->projection.top();
    case GL_TEXTURE: return c->texmat[c->st.active_unit].top();
    default: return c->modelview.top();
    }
}

static MatStack &cur_stack(Context *c)
{
    switch (c->st.matrix_mode) {
    case GL_PROJECTION: return c->projection;
    case GL_TEXTURE: return c->texmat[c->st.active_unit];
    default: return c->modelview;
    }
}

void matrix_changed(Context *c)
{
    if (c->st.matrix_mode != GL_TEXTURE) {
        c->mv_serial++;
    }
}

/* 改矩阵之前先把用旧矩阵攒的批次发掉 */
static void pre_change(Context *c)
{
    state_dirty(c);
}

static void load(Context *c, const Mat4 &m)
{
    pre_change(c);
    cur_matrix(c) = m;
    matrix_changed(c);
}

static void mult(Context *c, const Mat4 &m)
{
    pre_change(c);
    Mat4 &t = cur_matrix(c);
    t = mat_mul(t, m);
    matrix_changed(c);
}

void impl_glMatrixMode(GLenum mode)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glMatrixMode(mode));
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    c->st.matrix_mode = mode;
}

void impl_glLoadIdentity(void)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glLoadIdentity());
    load(c, Mat4::identity());
}

static Mat4 from_f(const GLfloat *m, bool transpose)
{
    Mat4 r;
    for (int i = 0; i < 16; i++) {
        r.m[i] = transpose ? m[(i % 4) * 4 + i / 4] : m[i];
    }
    return r;
}

static Mat4 from_d(const GLdouble *m, bool transpose)
{
    Mat4 r;
    for (int i = 0; i < 16; i++) {
        r.m[i] = transpose ? m[(i % 4) * 4 + i / 4] : m[i];
    }
    return r;
}

/* 指针参数的函数录制时必须拷贝数据 */
#define RECORD_MAT(c, fn, mat)                                                                    \
    if (list_recording(c)) {                                                                      \
        const Mat4 copy = (mat);                                                                  \
        list_record_fn(c, [=]() {                                                                 \
            Context *cc = cur();                                                                  \
            if (cc) {                                                                             \
                fn(cc, copy);                                                                     \
            }                                                                                     \
        });                                                                                       \
        if (list_compile_only(c)) {                                                               \
            return;                                                                               \
        }                                                                                         \
    }

void impl_glLoadMatrixf(const GLfloat *m)
{
    ESPGL_CTX(c);
    if (!m) return;
    const Mat4 mm = from_f(m, false);
    RECORD_MAT(c, load, mm);
    load(c, mm);
}

void impl_glLoadMatrixd(const GLdouble *m)
{
    ESPGL_CTX(c);
    if (!m) return;
    const Mat4 mm = from_d(m, false);
    RECORD_MAT(c, load, mm);
    load(c, mm);
}

void impl_glMultMatrixf(const GLfloat *m)
{
    ESPGL_CTX(c);
    if (!m) return;
    const Mat4 mm = from_f(m, false);
    RECORD_MAT(c, mult, mm);
    mult(c, mm);
}

void impl_glMultMatrixd(const GLdouble *m)
{
    ESPGL_CTX(c);
    if (!m) return;
    const Mat4 mm = from_d(m, false);
    RECORD_MAT(c, mult, mm);
    mult(c, mm);
}

void impl_glLoadTransposeMatrixf(const GLfloat *m)
{
    ESPGL_CTX(c);
    if (!m) return;
    const Mat4 mm = from_f(m, true);
    RECORD_MAT(c, load, mm);
    load(c, mm);
}

void impl_glLoadTransposeMatrixd(const GLdouble *m)
{
    ESPGL_CTX(c);
    if (!m) return;
    const Mat4 mm = from_d(m, true);
    RECORD_MAT(c, load, mm);
    load(c, mm);
}

void impl_glMultTransposeMatrixf(const GLfloat *m)
{
    ESPGL_CTX(c);
    if (!m) return;
    const Mat4 mm = from_f(m, true);
    RECORD_MAT(c, mult, mm);
    mult(c, mm);
}

void impl_glMultTransposeMatrixd(const GLdouble *m)
{
    ESPGL_CTX(c);
    if (!m) return;
    const Mat4 mm = from_d(m, true);
    RECORD_MAT(c, mult, mm);
    mult(c, mm);
}

static void do_translate(Context *c, double x, double y, double z)
{
    Mat4 t = Mat4::identity();
    t.m[12] = x;
    t.m[13] = y;
    t.m[14] = z;
    mult(c, t);
}

static void do_scale(Context *c, double x, double y, double z)
{
    Mat4 t = Mat4::identity();
    t.m[0] = x;
    t.m[5] = y;
    t.m[10] = z;
    mult(c, t);
}

static void do_rotate(Context *c, double angle, double x, double y, double z)
{
    const double len = std::sqrt(x * x + y * y + z * z);
    if (len == 0.0) {
        return;
    }
    x /= len;
    y /= len;
    z /= len;
    const double a = angle * 3.14159265358979323846 / 180.0;
    const double cs = std::cos(a), sn = std::sin(a), ic = 1.0 - cs;
    Mat4 r = Mat4::identity();
    r.m[0] = x * x * ic + cs;
    r.m[1] = y * x * ic + z * sn;
    r.m[2] = x * z * ic - y * sn;
    r.m[4] = x * y * ic - z * sn;
    r.m[5] = y * y * ic + cs;
    r.m[6] = y * z * ic + x * sn;
    r.m[8] = x * z * ic + y * sn;
    r.m[9] = y * z * ic - x * sn;
    r.m[10] = z * z * ic + cs;
    mult(c, r);
}

void impl_glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glTranslatef(x, y, z));
    do_translate(c, x, y, z);
}

void impl_glTranslated(GLdouble x, GLdouble y, GLdouble z)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glTranslated(x, y, z));
    do_translate(c, x, y, z);
}

void impl_glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glScalef(x, y, z));
    do_scale(c, x, y, z);
}

void impl_glScaled(GLdouble x, GLdouble y, GLdouble z)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glScaled(x, y, z));
    do_scale(c, x, y, z);
}

void impl_glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glRotatef(angle, x, y, z));
    do_rotate(c, angle, x, y, z);
}

void impl_glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glRotated(angle, x, y, z));
    do_rotate(c, angle, x, y, z);
}

void impl_glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glFrustum(l, r, b, t, n, f));
    if (n <= 0 || f <= 0 || l == r || b == t || n == f) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    Mat4 m{};
    m.m[0] = 2 * n / (r - l);
    m.m[5] = 2 * n / (t - b);
    m.m[8] = (r + l) / (r - l);
    m.m[9] = (t + b) / (t - b);
    m.m[10] = -(f + n) / (f - n);
    m.m[11] = -1;
    m.m[14] = -2 * f * n / (f - n);
    mult(c, m);
}

void impl_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glOrtho(l, r, b, t, n, f));
    if (l == r || b == t || n == f) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    Mat4 m = Mat4::identity();
    m.m[0] = 2 / (r - l);
    m.m[5] = 2 / (t - b);
    m.m[10] = -2 / (f - n);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -(f + n) / (f - n);
    mult(c, m);
}

void impl_glPushMatrix(void)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glPushMatrix());
    MatStack &s = cur_stack(c);
    if (s.s.size() >= s.max_depth) {
        set_error(GL_STACK_OVERFLOW);
        return;
    }
    s.s.push_back(s.top());
}

void impl_glPopMatrix(void)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glPopMatrix());
    MatStack &s = cur_stack(c);
    if (s.s.size() <= 1) {
        set_error(GL_STACK_UNDERFLOW);
        return;
    }
    pre_change(c);
    s.s.pop_back();
    matrix_changed(c);
}

} // namespace espgl
