/*
 * gl_vertex.cpp — 立即模式、当前属性、客户端顶点数组、图元归并
 *
 * 所有几何最终都走 emit()：把 GL 图元拆成设备能吃的四类（四边形 / 三角形 / 线 / 点）
 * 追加进 Pending。Doom 每面墙一个 4 顶点 strip，逐个发 DRAW 的话命令头比顶点还大，
 * 所以只要状态没变就一直往同一个批次里攒（4 顶点 strip / fan 直接当四边形，省掉索引）。
 */

#include "gl_context.h"

#include <algorithm>
#include <cmath>

namespace espgl {

/* ── 当前属性 ─────────────────────────────────────────────────────── */

void current_vertex(Context *c, Vertex &v)
{
    v.pos[0] = v.pos[1] = v.pos[2] = 0;
    v.pos[3] = 1;
    memcpy(v.color, c->st.cur_color, sizeof(v.color));
    memcpy(v.tc, c->st.cur_tc, sizeof(v.tc));
    memcpy(v.normal, c->st.cur_normal, sizeof(v.normal));
}

namespace {

struct RecAttrs {
    float color[4];
    float normal[3];
    float tc[kMaxUnits][4];
};

/* 每个上下文一份录制器属性（与 Recorder 同生命周期，挂在 thread_local 上即可：录制不跨线程） */
thread_local RecAttrs t_rec;

inline bool rec_active(Context *c)
{
    return c->rec != nullptr;
}

void apply_color_material(Context *c)
{
    State &s = c->st;
    if (!s.color_material) {
        return;
    }
    for (int f = 0; f < 2; f++) {
        if ((f == 0 && s.color_material_face == GL_BACK) || (f == 1 && s.color_material_face == GL_FRONT)) {
            continue;
        }
        MaterialState &m = s.material[f];
        switch (s.color_material_mode) {
        case GL_AMBIENT: memcpy(m.ambient, s.cur_color, 16); break;
        case GL_DIFFUSE: memcpy(m.diffuse, s.cur_color, 16); break;
        case GL_SPECULAR: memcpy(m.specular, s.cur_color, 16); break;
        case GL_EMISSION: memcpy(m.emission, s.cur_color, 16); break;
        default:
            memcpy(m.ambient, s.cur_color, 16);
            memcpy(m.diffuse, s.cur_color, 16);
            break;
        }
    }
}

void attr_color(Context *c, float r, float g, float b, float a)
{
    if (rec_active(c)) {
        Recorder &R = *c->rec;
        t_rec.color[0] = r;
        t_rec.color[1] = g;
        t_rec.color[2] = b;
        t_rec.color[3] = a;
        R.color_set = true;
        if (!R.in_begin) {
            list_record_fn(c, [=]() {
                if (Context *cc = cur()) {
                    attr_color(cc, r, g, b, a);
                }
            });
        }
        if (list_compile_only(c)) {
            return;
        }
    }
    float *cc = c->st.cur_color;
    cc[0] = r;
    cc[1] = g;
    cc[2] = b;
    cc[3] = a;
    /* 开着 COLOR_MATERIAL 时设备直接用顶点色，不需要 flush */
    apply_color_material(c);
}

void attr_normal(Context *c, float x, float y, float z)
{
    if (rec_active(c)) {
        Recorder &R = *c->rec;
        t_rec.normal[0] = x;
        t_rec.normal[1] = y;
        t_rec.normal[2] = z;
        R.normal_set = true;
        if (!R.in_begin) {
            list_record_fn(c, [=]() {
                if (Context *cc = cur()) {
                    attr_normal(cc, x, y, z);
                }
            });
        }
        if (list_compile_only(c)) {
            return;
        }
    }
    c->st.cur_normal[0] = x;
    c->st.cur_normal[1] = y;
    c->st.cur_normal[2] = z;
}

void attr_tc(Context *c, int unit, float s, float t, float r, float q)
{
    if (unit < 0 || unit >= kMaxUnits) {
        return;
    }
    if (rec_active(c)) {
        Recorder &R = *c->rec;
        t_rec.tc[unit][0] = s;
        t_rec.tc[unit][1] = t;
        t_rec.tc[unit][2] = r;
        t_rec.tc[unit][3] = q;
        R.tc_set[unit] = true;
        if (!R.in_begin) {
            list_record_fn(c, [=]() {
                if (Context *cc = cur()) {
                    attr_tc(cc, unit, s, t, r, q);
                }
            });
        }
        if (list_compile_only(c)) {
            return;
        }
    }
    float *d = c->st.cur_tc[unit];
    d[0] = s;
    d[1] = t;
    d[2] = r;
    d[3] = q;
}

void attr_vertex(Context *c, float x, float y, float z, float w)
{
    if (rec_active(c) && c->rec->in_begin) {
        Recorder &R = *c->rec;
        Vertex v;
        current_vertex(c, v);
        if (R.color_set) memcpy(v.color, t_rec.color, sizeof(v.color));
        if (R.normal_set) memcpy(v.normal, t_rec.normal, sizeof(v.normal));
        for (int u = 0; u < kMaxUnits; u++) {
            if (R.tc_set[u]) memcpy(v.tc[u], t_rec.tc[u], sizeof(v.tc[u]));
        }
        v.pos[0] = x;
        v.pos[1] = y;
        v.pos[2] = z;
        v.pos[3] = w;
        R.cur.v.push_back(v);
        if (list_compile_only(c)) {
            return;
        }
    }
    if (!c->in_begin) {
        return; /* begin 之外的 glVertex 无定义，忽略 */
    }
    Vertex v;
    current_vertex(c, v);
    v.pos[0] = x;
    v.pos[1] = y;
    v.pos[2] = z;
    v.pos[3] = w;
    c->im.push_back(v);
}

inline float nb(GLbyte v) { return (2.0f * v + 1.0f) / 255.0f; }
inline float nub(GLubyte v) { return v / 255.0f; }
inline float ns(GLshort v) { return (2.0f * v + 1.0f) / 65535.0f; }
inline float nus(GLushort v) { return v / 65535.0f; }
inline float ni(GLint v) { return static_cast<float>((2.0 * v + 1.0) / 4294967295.0); }
inline float nui(GLuint v) { return static_cast<float>(v / 4294967295.0); }

} // namespace

/* 给录制器用：NewList 时把录制属性初始化为当前值 */
void recorder_reset_attrs(Context *c)
{
    memcpy(t_rec.color, c->st.cur_color, sizeof(t_rec.color));
    memcpy(t_rec.normal, c->st.cur_normal, sizeof(t_rec.normal));
    memcpy(t_rec.tc, c->st.cur_tc, sizeof(t_rec.tc));
}

/* ── glBegin / glEnd ──────────────────────────────────────────────── */

void impl_glBegin(GLenum mode)
{
    ESPGL_CTX(c);
    if (mode > GL_POLYGON) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    if (rec_active(c)) {
        Recorder &R = *c->rec;
        R.in_begin = true;
        R.cur = ListBatch();
        R.cur.mode = mode;
        if (list_compile_only(c)) {
            return;
        }
    }
    if (c->in_begin) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    c->in_begin = true;
    c->begin_mode = mode;
    c->im.clear();
}

void list_push_batch(Context *c, ListBatch &&b);

void impl_glEnd(void)
{
    ESPGL_CTX(c);
    if (rec_active(c) && c->rec->in_begin) {
        Recorder &R = *c->rec;
        R.in_begin = false;
        R.cur.has_color = R.color_set;
        R.cur.has_normal = R.normal_set;
        for (int u = 0; u < kMaxUnits; u++) {
            R.cur.has_tc[u] = R.tc_set[u];
        }
        list_push_batch(c, std::move(R.cur));
        R.cur = ListBatch();
        if (list_compile_only(c)) {
            return;
        }
    }
    if (!c->in_begin) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    c->in_begin = false;
    if (!c->im.empty()) {
        emit(c, c->begin_mode, c->im.data(), c->im.size());
    }
    c->im.clear();
}

/* ── glVertex* ────────────────────────────────────────────────────── */

#define V2(fn, T)                                                                                  \
    void impl_##fn(T x, T y) { ESPGL_CTX(c); attr_vertex(c, (float)x, (float)y, 0, 1); }        \
    void impl_##fn##v(const T *v) { ESPGL_CTX(c); if (v) attr_vertex(c, (float)v[0], (float)v[1], 0, 1); }
#define V3(fn, T)                                                                                  \
    void impl_##fn(T x, T y, T z) { ESPGL_CTX(c); attr_vertex(c, (float)x, (float)y, (float)z, 1); } \
    void impl_##fn##v(const T *v) { ESPGL_CTX(c); if (v) attr_vertex(c, (float)v[0], (float)v[1], (float)v[2], 1); }
#define V4(fn, T)                                                                                  \
    void impl_##fn(T x, T y, T z, T w) { ESPGL_CTX(c); attr_vertex(c, (float)x, (float)y, (float)z, (float)w); } \
    void impl_##fn##v(const T *v) { ESPGL_CTX(c); if (v) attr_vertex(c, (float)v[0], (float)v[1], (float)v[2], (float)v[3]); }

V2(glVertex2d, GLdouble)
V2(glVertex2f, GLfloat)
V2(glVertex2i, GLint)
V2(glVertex2s, GLshort)
V3(glVertex3d, GLdouble)
V3(glVertex3f, GLfloat)
V3(glVertex3i, GLint)
V3(glVertex3s, GLshort)
V4(glVertex4d, GLdouble)
V4(glVertex4f, GLfloat)
V4(glVertex4i, GLint)
V4(glVertex4s, GLshort)

/* ── glColor* ─────────────────────────────────────────────────────── */

#define C3(fn, T, N)                                                                               \
    void impl_##fn(T r, T g, T b) { ESPGL_CTX(c); attr_color(c, N(r), N(g), N(b), 1.0f); }       \
    void impl_##fn##v(const T *v) { ESPGL_CTX(c); if (v) attr_color(c, N(v[0]), N(v[1]), N(v[2]), 1.0f); }
#define C4(fn, T, N)                                                                               \
    void impl_##fn(T r, T g, T b, T a) { ESPGL_CTX(c); attr_color(c, N(r), N(g), N(b), N(a)); }  \
    void impl_##fn##v(const T *v) { ESPGL_CTX(c); if (v) attr_color(c, N(v[0]), N(v[1]), N(v[2]), N(v[3])); }

#define NF(x) static_cast<float>(x)
C3(glColor3b, GLbyte, nb)
C3(glColor3d, GLdouble, NF)
C3(glColor3f, GLfloat, NF)
C3(glColor3i, GLint, ni)
C3(glColor3s, GLshort, ns)
C3(glColor3ub, GLubyte, nub)
C3(glColor3ui, GLuint, nui)
C3(glColor3us, GLushort, nus)
C4(glColor4b, GLbyte, nb)
C4(glColor4d, GLdouble, NF)
C4(glColor4f, GLfloat, NF)
C4(glColor4i, GLint, ni)
C4(glColor4s, GLshort, ns)
C4(glColor4ub, GLubyte, nub)
C4(glColor4ui, GLuint, nui)
C4(glColor4us, GLushort, nus)

/* ── glNormal* ────────────────────────────────────────────────────── */

#define N3(fn, T, N)                                                                               \
    void impl_##fn(T x, T y, T z) { ESPGL_CTX(c); attr_normal(c, N(x), N(y), N(z)); }            \
    void impl_##fn##v(const T *v) { ESPGL_CTX(c); if (v) attr_normal(c, N(v[0]), N(v[1]), N(v[2])); }

N3(glNormal3b, GLbyte, nb)
N3(glNormal3d, GLdouble, NF)
N3(glNormal3f, GLfloat, NF)
N3(glNormal3i, GLint, ni)
N3(glNormal3s, GLshort, ns)

/* ── glTexCoord* / glMultiTexCoord* ───────────────────────────────── */

#define T1(fn, T)                                                                                  \
    void impl_##fn(T s) { ESPGL_CTX(c); attr_tc(c, 0, (float)s, 0, 0, 1); }                      \
    void impl_##fn##v(const T *v) { ESPGL_CTX(c); if (v) attr_tc(c, 0, (float)v[0], 0, 0, 1); }
#define T2(fn, T)                                                                                  \
    void impl_##fn(T s, T t) { ESPGL_CTX(c); attr_tc(c, 0, (float)s, (float)t, 0, 1); }         \
    void impl_##fn##v(const T *v) { ESPGL_CTX(c); if (v) attr_tc(c, 0, (float)v[0], (float)v[1], 0, 1); }
#define T3(fn, T)                                                                                  \
    void impl_##fn(T s, T t, T r) { ESPGL_CTX(c); attr_tc(c, 0, (float)s, (float)t, (float)r, 1); } \
    void impl_##fn##v(const T *v) { ESPGL_CTX(c); if (v) attr_tc(c, 0, (float)v[0], (float)v[1], (float)v[2], 1); }
#define T4(fn, T)                                                                                  \
    void impl_##fn(T s, T t, T r, T q) { ESPGL_CTX(c); attr_tc(c, 0, (float)s, (float)t, (float)r, (float)q); } \
    void impl_##fn##v(const T *v) { ESPGL_CTX(c); if (v) attr_tc(c, 0, (float)v[0], (float)v[1], (float)v[2], (float)v[3]); }

T1(glTexCoord1d, GLdouble)
T1(glTexCoord1f, GLfloat)
T1(glTexCoord1i, GLint)
T1(glTexCoord1s, GLshort)
T2(glTexCoord2d, GLdouble)
T2(glTexCoord2f, GLfloat)
T2(glTexCoord2i, GLint)
T2(glTexCoord2s, GLshort)
T3(glTexCoord3d, GLdouble)
T3(glTexCoord3f, GLfloat)
T3(glTexCoord3i, GLint)
T3(glTexCoord3s, GLshort)
T4(glTexCoord4d, GLdouble)
T4(glTexCoord4f, GLfloat)
T4(glTexCoord4i, GLint)
T4(glTexCoord4s, GLshort)

#define UNIT(target) (static_cast<int>(target) - static_cast<int>(GL_TEXTURE0))
#define M1(fn, T)                                                                                  \
    void impl_##fn(GLenum tg, T s) { ESPGL_CTX(c); attr_tc(c, UNIT(tg), (float)s, 0, 0, 1); }    \
    void impl_##fn##v(GLenum tg, const T *v) { ESPGL_CTX(c); if (v) attr_tc(c, UNIT(tg), (float)v[0], 0, 0, 1); }
#define M2(fn, T)                                                                                  \
    void impl_##fn(GLenum tg, T s, T t) { ESPGL_CTX(c); attr_tc(c, UNIT(tg), (float)s, (float)t, 0, 1); } \
    void impl_##fn##v(GLenum tg, const T *v) { ESPGL_CTX(c); if (v) attr_tc(c, UNIT(tg), (float)v[0], (float)v[1], 0, 1); }
#define M3(fn, T)                                                                                  \
    void impl_##fn(GLenum tg, T s, T t, T r) { ESPGL_CTX(c); attr_tc(c, UNIT(tg), (float)s, (float)t, (float)r, 1); } \
    void impl_##fn##v(GLenum tg, const T *v) { ESPGL_CTX(c); if (v) attr_tc(c, UNIT(tg), (float)v[0], (float)v[1], (float)v[2], 1); }
#define M4(fn, T)                                                                                  \
    void impl_##fn(GLenum tg, T s, T t, T r, T q) { ESPGL_CTX(c); attr_tc(c, UNIT(tg), (float)s, (float)t, (float)r, (float)q); } \
    void impl_##fn##v(GLenum tg, const T *v) { ESPGL_CTX(c); if (v) attr_tc(c, UNIT(tg), (float)v[0], (float)v[1], (float)v[2], (float)v[3]); }

M1(glMultiTexCoord1d, GLdouble)
M1(glMultiTexCoord1f, GLfloat)
M1(glMultiTexCoord1i, GLint)
M1(glMultiTexCoord1s, GLshort)
M2(glMultiTexCoord2d, GLdouble)
M2(glMultiTexCoord2f, GLfloat)
M2(glMultiTexCoord2i, GLint)
M2(glMultiTexCoord2s, GLshort)
M3(glMultiTexCoord3d, GLdouble)
M3(glMultiTexCoord3f, GLfloat)
M3(glMultiTexCoord3i, GLint)
M3(glMultiTexCoord3s, GLshort)
M4(glMultiTexCoord4d, GLdouble)
M4(glMultiTexCoord4f, GLfloat)
M4(glMultiTexCoord4i, GLint)
M4(glMultiTexCoord4s, GLshort)

void impl_glActiveTexture(GLenum texture)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glActiveTexture(texture));
    const int u = UNIT(texture);
    if (u < 0 || u >= kMaxUnits) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    c->st.active_unit = u;
}

void impl_glClientActiveTexture(GLenum texture)
{
    ESPGL_CTX(c);
    const int u = UNIT(texture);
    if (u < 0 || u >= kMaxUnits) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    c->cs.client_active = u;
}

/* ── glRect* ──────────────────────────────────────────────────────── */

static void rect(Context *c, float x1, float y1, float x2, float y2)
{
    (void)c;
    impl_glBegin(GL_QUADS);
    impl_glVertex2f(x1, y1);
    impl_glVertex2f(x2, y1);
    impl_glVertex2f(x2, y2);
    impl_glVertex2f(x1, y2);
    impl_glEnd();
}

#define R4(fn, T)                                                                                  \
    void impl_##fn(T x1, T y1, T x2, T y2) { ESPGL_CTX(c); rect(c, (float)x1, (float)y1, (float)x2, (float)y2); } \
    void impl_##fn##v(const T *a, const T *b) { ESPGL_CTX(c); if (a && b) rect(c, (float)a[0], (float)a[1], (float)b[0], (float)b[1]); }

R4(glRectd, GLdouble)
R4(glRectf, GLfloat)
R4(glRecti, GLint)
R4(glRects, GLshort)

/* ── 客户端数组 ───────────────────────────────────────────────────── */

static int type_size(GLenum type)
{
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT: return 2;
    case GL_INT:
    case GL_UNSIGNED_INT:
    case GL_FLOAT: return 4;
    case GL_DOUBLE: return 8;
    default: return 0;
    }
}

static void set_array(ArrayState &a, GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    a.size = size;
    a.type = type;
    a.stride = stride;
    a.ptr = ptr;
}

void impl_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    ESPGL_CTX(c);
    set_array(c->cs.vertex, size, type, stride, ptr);
}

void impl_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    ESPGL_CTX(c);
    set_array(c->cs.color, size, type, stride, ptr);
}

void impl_glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr)
{
    ESPGL_CTX(c);
    set_array(c->cs.normal, 3, type, stride, ptr);
}

void impl_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    ESPGL_CTX(c);
    set_array(c->cs.tc[c->cs.client_active], size, type, stride, ptr);
}

void impl_glIndexPointer(GLenum type, GLsizei stride, const GLvoid *ptr)
{
    ESPGL_CTX(c);
    set_array(c->cs.index, 1, type, stride, ptr);
}

void impl_glEdgeFlagPointer(GLsizei stride, const GLvoid *ptr)
{
    ESPGL_CTX(c);
    set_array(c->cs.edge, 1, GL_UNSIGNED_BYTE, stride, ptr);
}

void impl_glLockArraysEXT(GLint first, GLsizei count)
{
    (void)first;
    (void)count;
    ESPGL_CTX(c);
    c->arrays_locked = true;
}

void impl_glUnlockArraysEXT(void)
{
    ESPGL_CTX(c);
    c->arrays_locked = false;
}

/** 读第 i 个元素的 n 个分量（n <= 4），normalize 用于颜色 / 法线的整数类型 */
static void read_comps(const ArrayState &a, GLint i, int n, bool normalize, float *out)
{
    const int ts = type_size(a.type);
    if (ts == 0 || a.ptr == nullptr) {
        return;
    }
    const int comps = a.size;
    const size_t stride = a.stride ? static_cast<size_t>(a.stride) : static_cast<size_t>(comps * ts);
    const uint8_t *p = static_cast<const uint8_t *>(a.ptr) + stride * static_cast<size_t>(i);
    const int m = std::min(comps, n);
    for (int k = 0; k < m; k++) {
        const uint8_t *q = p + k * ts;
        float v = 0;
        switch (a.type) {
        case GL_BYTE: { GLbyte t; memcpy(&t, q, 1); v = normalize ? nb(t) : t; break; }
        case GL_UNSIGNED_BYTE: { GLubyte t = *q; v = normalize ? nub(t) : t; break; }
        case GL_SHORT: { GLshort t; memcpy(&t, q, 2); v = normalize ? ns(t) : t; break; }
        case GL_UNSIGNED_SHORT: { GLushort t; memcpy(&t, q, 2); v = normalize ? nus(t) : t; break; }
        case GL_INT: { GLint t; memcpy(&t, q, 4); v = normalize ? ni(t) : static_cast<float>(t); break; }
        case GL_UNSIGNED_INT: { GLuint t; memcpy(&t, q, 4); v = normalize ? nui(t) : static_cast<float>(t); break; }
        case GL_FLOAT: memcpy(&v, q, 4); break;
        case GL_DOUBLE: { double t; memcpy(&t, q, 8); v = static_cast<float>(t); break; }
        default: break;
        }
        out[k] = v;
    }
}

void fetch_array_vertex(Context *c, GLint i, Vertex &v)
{
    current_vertex(c, v);
    const ClientState &cs = c->cs;
    if (cs.color.enabled) {
        float col[4] = {v.color[0], v.color[1], v.color[2], 1.0f};
        read_comps(cs.color, i, 4, cs.color.type != GL_FLOAT && cs.color.type != GL_DOUBLE, col);
        if (cs.color.size == 3) {
            col[3] = 1.0f;
        }
        memcpy(v.color, col, sizeof(col));
    }
    if (cs.normal.enabled) {
        read_comps(cs.normal, i, 3, cs.normal.type != GL_FLOAT && cs.normal.type != GL_DOUBLE, v.normal);
    }
    for (int u = 0; u < kMaxUnits; u++) {
        if (cs.tc[u].enabled) {
            float t[4] = {0, 0, 0, 1};
            read_comps(cs.tc[u], i, 4, false, t);
            memcpy(v.tc[u], t, sizeof(t));
        }
    }
    if (cs.vertex.enabled) {
        float p[4] = {0, 0, 0, 1};
        read_comps(cs.vertex, i, 4, false, p);
        memcpy(v.pos, p, sizeof(p));
    }
}

void impl_glArrayElement(GLint i)
{
    ESPGL_CTX(c);
    Vertex v;
    fetch_array_vertex(c, i, v);
    if (rec_active(c) && c->rec->in_begin) {
        Recorder &R = *c->rec;
        if (c->cs.color.enabled) R.color_set = true;
        if (c->cs.normal.enabled) R.normal_set = true;
        for (int u = 0; u < kMaxUnits; u++) {
            if (c->cs.tc[u].enabled) R.tc_set[u] = true;
        }
        R.cur.v.push_back(v);
        if (list_compile_only(c)) {
            return;
        }
    }
    if (c->in_begin) {
        c->im.push_back(v);
    }
}

static void record_array_batch(Context *c, GLenum mode, std::vector<Vertex> &&v)
{
    ListBatch b;
    b.mode = mode;
    b.v = std::move(v);
    b.has_color = c->cs.color.enabled || c->rec->color_set;
    b.has_normal = c->cs.normal.enabled || c->rec->normal_set;
    for (int u = 0; u < kMaxUnits; u++) {
        b.has_tc[u] = c->cs.tc[u].enabled || c->rec->tc_set[u];
    }
    list_push_batch(c, std::move(b));
}

void impl_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    ESPGL_CTX(c);
    if (count <= 0 || !c->cs.vertex.enabled) {
        return;
    }
    std::vector<Vertex> v(static_cast<size_t>(count));
    for (GLsizei k = 0; k < count; k++) {
        fetch_array_vertex(c, first + k, v[static_cast<size_t>(k)]);
    }
    if (rec_active(c)) {
        std::vector<Vertex> copy = v;
        record_array_batch(c, mode, std::move(copy));
        if (list_compile_only(c)) {
            return;
        }
    }
    emit(c, mode, v.data(), v.size());
}

static bool read_indices(GLsizei count, GLenum type, const void *indices, std::vector<uint32_t> &out)
{
    if (indices == nullptr || count <= 0) {
        return false;
    }
    out.resize(static_cast<size_t>(count));
    switch (type) {
    case GL_UNSIGNED_BYTE: {
        const auto *p = static_cast<const GLubyte *>(indices);
        for (GLsizei i = 0; i < count; i++) out[static_cast<size_t>(i)] = p[i];
        return true;
    }
    case GL_UNSIGNED_SHORT: {
        const auto *p = static_cast<const GLushort *>(indices);
        for (GLsizei i = 0; i < count; i++) out[static_cast<size_t>(i)] = p[i];
        return true;
    }
    case GL_UNSIGNED_INT: {
        const auto *p = static_cast<const GLuint *>(indices);
        for (GLsizei i = 0; i < count; i++) out[static_cast<size_t>(i)] = p[i];
        return true;
    }
    default: set_error(GL_INVALID_ENUM); return false;
    }
}

static void draw_elements(Context *c, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    if (!c->cs.vertex.enabled) {
        return;
    }
    std::vector<uint32_t> idx;
    if (!read_indices(count, type, indices, idx)) {
        return;
    }
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t i : idx) {
        lo = std::min(lo, i);
        hi = std::max(hi, i);
    }
    const size_t range = static_cast<size_t>(hi - lo) + 1;

    /* 录制或区间稀疏时直接展开成非索引 */
    if (rec_active(c) || range > idx.size() * 2 || range > 4000 ||
        (mode != GL_TRIANGLES && mode != GL_LINES && mode != GL_POINTS)) {
        std::vector<Vertex> v(idx.size());
        for (size_t k = 0; k < idx.size(); k++) {
            fetch_array_vertex(c, static_cast<GLint>(idx[k]), v[k]);
        }
        if (rec_active(c)) {
            std::vector<Vertex> copy = v;
            record_array_batch(c, mode, std::move(copy));
            if (list_compile_only(c)) {
                return;
            }
        }
        emit(c, mode, v.data(), v.size());
        return;
    }

    std::vector<Vertex> v(range);
    for (size_t k = 0; k < range; k++) {
        fetch_array_vertex(c, static_cast<GLint>(lo + k), v[k]);
    }
    for (uint32_t &i : idx) {
        i -= lo;
    }
    emit_indexed(c, mode, v.data(), v.size(), idx.data(), idx.size());
}

void impl_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    ESPGL_CTX(c);
    draw_elements(c, mode, count, type, indices);
}

void impl_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                              const GLvoid *indices)
{
    (void)start;
    (void)end;
    ESPGL_CTX(c);
    draw_elements(c, mode, count, type, indices);
}

void impl_glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer)
{
    ESPGL_CTX(c);
    /* 每种格式：tc 分量数 / color 分量数与类型 / 是否有法线 / 位置分量数 */
    int tc = 0, cc = 0, nn = 0, vc = 0;
    GLenum ctype = GL_FLOAT;
    switch (format) {
    case GL_V2F: vc = 2; break;
    case GL_V3F: vc = 3; break;
    case GL_C4UB_V2F: cc = 4; ctype = GL_UNSIGNED_BYTE; vc = 2; break;
    case GL_C4UB_V3F: cc = 4; ctype = GL_UNSIGNED_BYTE; vc = 3; break;
    case GL_C3F_V3F: cc = 3; vc = 3; break;
    case GL_N3F_V3F: nn = 1; vc = 3; break;
    case GL_C4F_N3F_V3F: cc = 4; nn = 1; vc = 3; break;
    case GL_T2F_V3F: tc = 2; vc = 3; break;
    case GL_T4F_V4F: tc = 4; vc = 4; break;
    case GL_T2F_C4UB_V3F: tc = 2; cc = 4; ctype = GL_UNSIGNED_BYTE; vc = 3; break;
    case GL_T2F_C3F_V3F: tc = 2; cc = 3; vc = 3; break;
    case GL_T2F_N3F_V3F: tc = 2; nn = 1; vc = 3; break;
    case GL_T2F_C4F_N3F_V3F: tc = 2; cc = 4; nn = 1; vc = 3; break;
    case GL_T4F_C4F_N3F_V4F: tc = 4; cc = 4; nn = 1; vc = 4; break;
    default: set_error(GL_INVALID_ENUM); return;
    }
    const int csize = cc ? (ctype == GL_UNSIGNED_BYTE ? 4 : cc * 4) : 0;
    const int total = tc * 4 + csize + nn * 12 + vc * 4;
    const GLsizei s = stride ? stride : total;
    const auto *base = static_cast<const uint8_t *>(pointer);
    int off = 0;
    ClientState &cs = c->cs;
    ArrayState &t = cs.tc[cs.client_active];
    t.enabled = tc != 0;
    if (tc) {
        set_array(t, tc, GL_FLOAT, s, base + off);
        off += tc * 4;
    }
    cs.color.enabled = cc != 0;
    if (cc) {
        set_array(cs.color, cc, ctype, s, base + off);
        off += csize;
    }
    cs.normal.enabled = nn != 0;
    if (nn) {
        set_array(cs.normal, 3, GL_FLOAT, s, base + off);
        off += 12;
    }
    cs.vertex.enabled = true;
    set_array(cs.vertex, vc, GL_FLOAT, s, base + off);
    cs.index.enabled = false;
    cs.edge.enabled = false;
}

/* ── 图元归并 ─────────────────────────────────────────────────────── */

namespace {

constexpr size_t kPendingMaxVerts = 6000;

bool reorder_ok(Context *c)
{
    const State &s = c->st;
    return s.depth_test && s.depth_mask && !s.blend &&
           (s.depth_func == GL_LESS || s.depth_func == GL_LEQUAL);
}

void quads_to_tris(Pending &p)
{
    for (size_t q = 0; q + 3 < p.quads.size(); q += 4) {
        const uint16_t b = static_cast<uint16_t>(p.tri_v.size());
        for (int k = 0; k < 4; k++) {
            p.tri_v.push_back(p.quads[q + static_cast<size_t>(k)]);
        }
        const uint16_t ix[6] = {b, static_cast<uint16_t>(b + 1), static_cast<uint16_t>(b + 2),
                                b, static_cast<uint16_t>(b + 2), static_cast<uint16_t>(b + 3)};
        p.tri_i.insert(p.tri_i.end(), ix, ix + 6);
    }
    p.quads.clear();
    p.classes &= ~(1 << PCLS_QUADS);
    if (!p.tri_i.empty()) {
        p.classes |= 1 << PCLS_TRIS;
    }
}

/** 准备往 pending 里加 cls 类图元 n 个顶点；返回实际应使用的类别 */
int prepare(Context *c, int cls, size_t n)
{
    Pending &p = c->pending;
    if (p.total() + n > kPendingMaxVerts) {
        flush_pending(c);
    }
    const int others = p.classes & ~(1 << cls);
    if (others == 0 || reorder_ok(c)) {
        return cls;
    }
    /* 不允许重排：填充类只保留一种 */
    const int fill = (1 << PCLS_QUADS) | (1 << PCLS_TRIS);
    if ((1 << cls) & fill) {
        if ((others & ~fill) == 0) {
            if (cls == PCLS_QUADS) {
                return PCLS_TRIS; /* 已有三角形：四边形也按三角形加，保持顺序 */
            }
            quads_to_tris(p); /* 已有四边形：转成三角形，后续一起按序画 */
            return PCLS_TRIS;
        }
    }
    flush_pending(c);
    return cls;
}

void add_quad(Context *c, const Vertex &a, const Vertex &b, const Vertex &cc, const Vertex &d)
{
    const int cls = prepare(c, PCLS_QUADS, 4);
    Pending &p = c->pending;
    if (cls == PCLS_QUADS) {
        p.quads.push_back(a);
        p.quads.push_back(b);
        p.quads.push_back(cc);
        p.quads.push_back(d);
        p.classes |= 1 << PCLS_QUADS;
        return;
    }
    const uint16_t base = static_cast<uint16_t>(p.tri_v.size());
    p.tri_v.push_back(a);
    p.tri_v.push_back(b);
    p.tri_v.push_back(cc);
    p.tri_v.push_back(d);
    const uint16_t ix[6] = {base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
                            base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)};
    p.tri_i.insert(p.tri_i.end(), ix, ix + 6);
    p.classes |= 1 << PCLS_TRIS;
}

/** 追加一组顶点 + 三角形索引（索引相对 v） */
void add_tris(Context *c, const Vertex *v, size_t nv, const uint32_t *idx, size_t ni)
{
    prepare(c, PCLS_TRIS, nv);
    Pending &p = c->pending;
    const uint32_t base = static_cast<uint32_t>(p.tri_v.size());
    p.tri_v.insert(p.tri_v.end(), v, v + nv);
    for (size_t k = 0; k < ni; k++) {
        p.tri_i.push_back(static_cast<uint16_t>(base + idx[k]));
    }
    p.classes |= 1 << PCLS_TRIS;
}

void add_lines(Context *c, const Vertex *v, size_t nv, const uint32_t *idx, size_t ni)
{
    prepare(c, PCLS_LINES, nv);
    Pending &p = c->pending;
    const uint32_t base = static_cast<uint32_t>(p.line_v.size());
    p.line_v.insert(p.line_v.end(), v, v + nv);
    for (size_t k = 0; k < ni; k++) {
        p.line_i.push_back(static_cast<uint16_t>(base + idx[k]));
    }
    p.classes |= 1 << PCLS_LINES;
}

void add_points(Context *c, const Vertex *v, size_t nv)
{
    prepare(c, PCLS_POINTS, nv);
    Pending &p = c->pending;
    p.points.insert(p.points.end(), v, v + nv);
    p.classes |= 1 << PCLS_POINTS;
}

} // namespace

void emit(Context *c, GLenum mode, const Vertex *v, size_t n)
{
    if (n == 0) {
        return;
    }
    /* 超大的单个图元按块切开，保证 u16 索引与 pending 上限 */
    if (n > kPendingMaxVerts / 2) {
        switch (mode) {
        case GL_POINTS:
        case GL_LINES:
        case GL_TRIANGLES:
        case GL_QUADS: {
            const size_t unit = mode == GL_POINTS ? 1 : mode == GL_LINES ? 2 : mode == GL_TRIANGLES ? 3 : 4;
            const size_t chunk = (kPendingMaxVerts / 2 / unit) * unit;
            for (size_t off = 0; off < n; off += chunk) {
                emit(c, mode, v + off, std::min(chunk, n - off));
            }
            return;
        }
        default: {
            /* 超长 strip / fan：先展开成列表再切块，避免 u16 索引越界 */
            std::vector<Vertex> flat;
            GLenum list_mode = GL_TRIANGLES;
            if (mode == GL_LINE_STRIP || mode == GL_LINE_LOOP) {
                list_mode = GL_LINES;
                for (size_t i = 0; i + 1 < n; i++) {
                    flat.push_back(v[i]);
                    flat.push_back(v[i + 1]);
                }
                if (mode == GL_LINE_LOOP) {
                    flat.push_back(v[n - 1]);
                    flat.push_back(v[0]);
                }
            } else if (mode == GL_TRIANGLE_STRIP) {
                for (size_t i = 0; i + 2 < n; i++) {
                    flat.push_back(v[(i & 1) ? i + 1 : i]);
                    flat.push_back(v[(i & 1) ? i : i + 1]);
                    flat.push_back(v[i + 2]);
                }
            } else if (mode == GL_QUAD_STRIP) {
                list_mode = GL_QUADS;
                for (size_t i = 0; i + 3 < n; i += 2) {
                    flat.push_back(v[i]);
                    flat.push_back(v[i + 1]);
                    flat.push_back(v[i + 3]);
                    flat.push_back(v[i + 2]);
                }
            } else { /* FAN / POLYGON */
                for (size_t i = 1; i + 1 < n; i++) {
                    flat.push_back(v[0]);
                    flat.push_back(v[i]);
                    flat.push_back(v[i + 1]);
                }
            }
            emit(c, list_mode, flat.data(), flat.size());
            return;
        }
        }
    }

    std::vector<uint32_t> idx;
    switch (mode) {
    case GL_POINTS:
        add_points(c, v, n);
        return;
    case GL_LINES:
        for (size_t i = 0; i + 1 < n; i += 2) {
            idx.push_back(static_cast<uint32_t>(i));
            idx.push_back(static_cast<uint32_t>(i + 1));
        }
        break;
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        for (size_t i = 0; i + 1 < n; i++) {
            idx.push_back(static_cast<uint32_t>(i));
            idx.push_back(static_cast<uint32_t>(i + 1));
        }
        if (mode == GL_LINE_LOOP && n > 2) {
            idx.push_back(static_cast<uint32_t>(n - 1));
            idx.push_back(0);
        }
        break;
    case GL_TRIANGLES:
        for (size_t i = 0; i + 2 < n; i += 3) {
            idx.push_back(static_cast<uint32_t>(i));
            idx.push_back(static_cast<uint32_t>(i + 1));
            idx.push_back(static_cast<uint32_t>(i + 2));
        }
        add_tris(c, v, n, idx.data(), idx.size());
        return;
    case GL_TRIANGLE_STRIP:
        if (n == 4) {
            add_quad(c, v[0], v[1], v[3], v[2]);
            return;
        }
        for (size_t i = 0; i + 2 < n; i++) {
            if (i & 1) {
                idx.push_back(static_cast<uint32_t>(i + 1));
                idx.push_back(static_cast<uint32_t>(i));
            } else {
                idx.push_back(static_cast<uint32_t>(i));
                idx.push_back(static_cast<uint32_t>(i + 1));
            }
            idx.push_back(static_cast<uint32_t>(i + 2));
        }
        add_tris(c, v, n, idx.data(), idx.size());
        return;
    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
        if (n == 4) {
            add_quad(c, v[0], v[1], v[2], v[3]);
            return;
        }
        for (size_t i = 1; i + 1 < n; i++) {
            idx.push_back(0);
            idx.push_back(static_cast<uint32_t>(i));
            idx.push_back(static_cast<uint32_t>(i + 1));
        }
        add_tris(c, v, n, idx.data(), idx.size());
        return;
    case GL_QUADS:
        for (size_t i = 0; i + 3 < n; i += 4) {
            add_quad(c, v[i], v[i + 1], v[i + 2], v[i + 3]);
        }
        return;
    case GL_QUAD_STRIP:
        for (size_t i = 0; i + 3 < n; i += 2) {
            add_quad(c, v[i], v[i + 1], v[i + 3], v[i + 2]);
        }
        return;
    default:
        return;
    }
    if (!idx.empty()) {
        add_lines(c, v, n, idx.data(), idx.size());
    }
}

void emit_indexed(Context *c, GLenum mode, const Vertex *v, size_t nv, const uint32_t *idx, size_t ni)
{
    switch (mode) {
    case GL_TRIANGLES:
        add_tris(c, v, nv, idx, ni - ni % 3);
        return;
    case GL_LINES:
        add_lines(c, v, nv, idx, ni - ni % 2);
        return;
    default: {
        std::vector<Vertex> flat(ni);
        for (size_t k = 0; k < ni; k++) {
            flat[k] = v[idx[k]];
        }
        emit(c, mode, flat.data(), flat.size());
        return;
    }
    }
}

void state_dirty(Context *c)
{
    if (!c->pending.empty()) {
        flush_pending(c);
    }
}

} // namespace espgl
