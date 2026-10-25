/*
 * gl_state.cpp — 状态设置 / 查询 / glEnable / glPushAttrib / glGetString
 */

#include "gl_context.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace espgl {

Shared g_sh;

namespace {
thread_local Context *t_cur = nullptr;

template <typename T>
inline void set_if(Context *c, T &field, const T &v)
{
    if (!(field == v)) {
        state_changed(c);
        field = v;
    }
}

inline void set4(Context *c, float *dst, const float *src)
{
    if (memcmp(dst, src, 4 * sizeof(float)) != 0) {
        state_changed(c);
        memcpy(dst, src, 4 * sizeof(float));
    }
}

inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

const char *kExtensions =
    "GL_ARB_multitexture "
    "GL_ARB_texture_env_combine "
    "GL_ARB_texture_env_add "
    "GL_ARB_texture_non_power_of_two "
    "GL_ARB_transpose_matrix "
    "GL_EXT_texture_env_combine "
    "GL_EXT_texture_env_add "
    "GL_EXT_paletted_texture "
    "GL_EXT_shared_texture_palette "
    "GL_EXT_bgra "
    "GL_EXT_texture_edge_clamp "
    "GL_SGIS_texture_edge_clamp "
    "GL_EXT_texture_object "
    "GL_EXT_vertex_array "
    "GL_EXT_compiled_vertex_array "
    "GL_EXT_draw_range_elements "
    "GL_EXT_blend_minmax "
    "GL_EXT_blend_subtract "
    "GL_EXT_blend_color "
    "GL_EXT_packed_pixels "
    "GL_ARB_texture_mirrored_repeat "
    "GL_IBM_texture_mirrored_repeat";

} // namespace

Context *cur()
{
    return t_cur;
}

void set_cur(Context *c)
{
    t_cur = c;
}

void set_error(GLenum e)
{
    Context *c = cur();
    if (c && c->error == GL_NO_ERROR) {
        c->error = e;
    }
}

int env_int(const char *name, int def)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return def;
    }
    return atoi(buf);
}

/* ── glEnable / glDisable ─────────────────────────────────────────── */

static bool *cap_ptr(Context *c, GLenum cap)
{
    State &s = c->st;
    TexUnitState &u = s.unit[s.active_unit];
    switch (cap) {
    case GL_BLEND: return &s.blend;
    case GL_ALPHA_TEST: return &s.alpha_test;
    case GL_DEPTH_TEST: return &s.depth_test;
    case GL_CULL_FACE: return &s.cull;
    case GL_FOG: return &s.fog;
    case GL_LIGHTING: return &s.lighting;
    case GL_SCISSOR_TEST: return &s.scissor;
    case GL_COLOR_MATERIAL: return &s.color_material;
    case GL_NORMALIZE: return &s.normalize;
    case 0x803A /* GL_RESCALE_NORMAL */: return &s.rescale_normal;
    case GL_TEXTURE_2D: return &u.enable_2d;
    case GL_TEXTURE_1D: return &u.enable_1d;
    case GL_POLYGON_OFFSET_FILL: return &s.polygon_offset_fill;
    case GL_POLYGON_OFFSET_LINE: return &s.polygon_offset_line;
    case GL_POLYGON_OFFSET_POINT: return &s.polygon_offset_point;
    case GL_DITHER: return &s.dither;
    case GL_LINE_SMOOTH: return &s.line_smooth;
    case GL_POINT_SMOOTH: return &s.point_smooth;
    case GL_POLYGON_SMOOTH: return &s.polygon_smooth;
    case GL_LINE_STIPPLE: return &s.line_stipple;
    case GL_POLYGON_STIPPLE: return &s.polygon_stipple;
    case GL_STENCIL_TEST: return &s.stencil_test;
    case GL_COLOR_LOGIC_OP: return &s.logic_op;
    case GL_TEXTURE_GEN_S: return &u.texgen[0];
    case GL_TEXTURE_GEN_T: return &u.texgen[1];
    case GL_TEXTURE_GEN_R: return &u.texgen[2];
    case GL_TEXTURE_GEN_Q: return &u.texgen[3];
    case 0x81FB /* GL_SHARED_TEXTURE_PALETTE_EXT */: return &s.shared_palette;
    default: break;
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + kMaxLights) {
        return &s.lights[cap - GL_LIGHT0].enabled;
    }
    if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + 6) {
        return &s.clip_plane[cap - GL_CLIP_PLANE0];
    }
    return nullptr;
}

void impl_glEnable(GLenum cap)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glEnable(cap));
    bool *p = cap_ptr(c, cap);
    if (p == nullptr) {
        return; /* 未知能力位：静默接受（不少老程序会开 GL_AUTO_NORMAL 之类） */
    }
    set_if(c, *p, true);
}

void impl_glDisable(GLenum cap)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glDisable(cap));
    bool *p = cap_ptr(c, cap);
    if (p == nullptr) {
        return;
    }
    set_if(c, *p, false);
}

static ArrayState *client_array(Context *c, GLenum array)
{
    switch (array) {
    case GL_VERTEX_ARRAY: return &c->cs.vertex;
    case GL_NORMAL_ARRAY: return &c->cs.normal;
    case GL_COLOR_ARRAY: return &c->cs.color;
    case GL_INDEX_ARRAY: return &c->cs.index;
    case GL_EDGE_FLAG_ARRAY: return &c->cs.edge;
    case GL_TEXTURE_COORD_ARRAY: return &c->cs.tc[c->cs.client_active];
    default: return nullptr;
    }
}

void impl_glEnableClientState(GLenum array)
{
    ESPGL_CTX(c);
    if (ArrayState *a = client_array(c, array)) {
        a->enabled = true;
    }
}

void impl_glDisableClientState(GLenum array)
{
    ESPGL_CTX(c);
    if (ArrayState *a = client_array(c, array)) {
        a->enabled = false;
    }
}

GLboolean impl_glIsEnabled(GLenum cap)
{
    ESPGL_CTX_RET(c, 0);
    if (bool *p = cap_ptr(c, cap)) {
        return *p ? 1 : 0;
    }
    if (ArrayState *a = client_array(c, cap)) {
        return a->enabled ? 1 : 0;
    }
    return 0;
}

/* ── 简单状态 ─────────────────────────────────────────────────────── */

void impl_glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glBlendFunc(sfactor, dfactor));
    set_if(c, c->st.blend_src, sfactor);
    set_if(c, c->st.blend_dst, dfactor);
}

void impl_glBlendEquation(GLenum mode)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glBlendEquation(mode));
    set_if(c, c->st.blend_eq, mode);
}

void impl_glBlendColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glBlendColor(r, g, b, a));
    const float v[4] = {r, g, b, a};
    set4(c, c->st.blend_color, v);
}

void impl_glAlphaFunc(GLenum func, GLclampf ref)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glAlphaFunc(func, ref));
    set_if(c, c->st.alpha_func, func);
    set_if(c, c->st.alpha_ref, clamp01(ref));
}

void impl_glDepthFunc(GLenum func)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glDepthFunc(func));
    set_if(c, c->st.depth_func, func);
}

void impl_glDepthMask(GLboolean flag)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glDepthMask(flag));
    set_if(c, c->st.depth_mask, flag != 0);
}

void impl_glDepthRange(GLclampd n, GLclampd f)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glDepthRange(n, f));
    const double nn = n < 0 ? 0 : (n > 1 ? 1 : n);
    const double ff = f < 0 ? 0 : (f > 1 ? 1 : f);
    if (c->st.depth_range[0] != nn || c->st.depth_range[1] != ff) {
        state_changed(c);
        c->st.depth_range[0] = nn;
        c->st.depth_range[1] = ff;
    }
}

void impl_glCullFace(GLenum mode)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glCullFace(mode));
    set_if(c, c->st.cull_face, mode);
}

void impl_glFrontFace(GLenum mode)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glFrontFace(mode));
    set_if(c, c->st.front_face, mode);
}

void impl_glShadeModel(GLenum mode)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glShadeModel(mode));
    set_if(c, c->st.shade_model, mode);
}

void impl_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glColorMask(r, g, b, a));
    const bool v[4] = {r != 0, g != 0, b != 0, a != 0};
    if (memcmp(v, c->st.color_mask, sizeof(v)) != 0) {
        state_changed(c);
        memcpy(c->st.color_mask, v, sizeof(v));
    }
}

void impl_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glClearColor(r, g, b, a));
    c->st.clear_color[0] = clamp01(r);
    c->st.clear_color[1] = clamp01(g);
    c->st.clear_color[2] = clamp01(b);
    c->st.clear_color[3] = clamp01(a);
}

void impl_glClearDepth(GLclampd d)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glClearDepth(d));
    c->st.clear_depth = d < 0 ? 0 : (d > 1 ? 1 : d);
}

void impl_glClearStencil(GLint s)
{
    ESPGL_CTX(c);
    c->st.clear_stencil = s;
}

void impl_glClearIndex(GLfloat v)
{
    (void)v;
}

void impl_glClearAccum(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    (void)r;
    (void)g;
    (void)b;
    (void)a;
}

void impl_glAccum(GLenum op, GLfloat value)
{
    (void)op;
    (void)value;
}

void impl_glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
    ESPGL_CTX(c);
    c->st.stencil_func = func;
    c->st.stencil_ref = ref;
    c->st.stencil_mask = mask;
}

void impl_glStencilMask(GLuint mask)
{
    ESPGL_CTX(c);
    c->st.stencil_writemask = mask;
}

void impl_glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
    ESPGL_CTX(c);
    c->st.stencil_fail = fail;
    c->st.stencil_zfail = zfail;
    c->st.stencil_zpass = zpass;
}

void impl_glLogicOp(GLenum opcode)
{
    ESPGL_CTX(c);
    c->st.logic_op_mode = opcode;
}

void impl_glPolygonMode(GLenum face, GLenum mode)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glPolygonMode(face, mode));
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) {
        c->st.polygon_mode[0] = mode;
    }
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) {
        c->st.polygon_mode[1] = mode;
    }
}

void impl_glPolygonOffset(GLfloat factor, GLfloat units)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glPolygonOffset(factor, units));
    set_if(c, c->st.offset_factor, factor);
    set_if(c, c->st.offset_units, units);
}

void impl_glPolygonStipple(const GLubyte *mask)
{
    (void)mask;
}

void impl_glGetPolygonStipple(GLubyte *mask)
{
    if (mask) {
        memset(mask, 0xFF, 128);
    }
}

void impl_glLineStipple(GLint factor, GLushort pattern)
{
    ESPGL_CTX(c);
    c->st.line_stipple_factor = factor;
    c->st.line_stipple_pattern = pattern;
}

void impl_glLineWidth(GLfloat width)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glLineWidth(width));
    set_if(c, c->st.line_width, width < 1.0f ? 1.0f : width);
}

void impl_glPointSize(GLfloat size)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glPointSize(size));
    set_if(c, c->st.point_size, size < 1.0f ? 1.0f : size);
}

void impl_glPointParameterf(GLenum pname, GLfloat param)
{
    (void)pname;
    (void)param;
}

void impl_glPointParameterfv(GLenum pname, const GLfloat *params)
{
    (void)pname;
    (void)params;
}

void impl_glHint(GLenum target, GLenum mode)
{
    ESPGL_CTX(c);
    switch (target) {
    case GL_PERSPECTIVE_CORRECTION_HINT: c->st.hint_perspective = mode; break;
    case GL_FOG_HINT: c->st.hint_fog = mode; break;
    case GL_LINE_SMOOTH_HINT: c->st.hint_line = mode; break;
    case GL_POINT_SMOOTH_HINT: c->st.hint_point = mode; break;
    case GL_POLYGON_SMOOTH_HINT: c->st.hint_polygon = mode; break;
    default: break;
    }
}

void impl_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glViewport(x, y, w, h));
    if (w < 0 || h < 0) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    const GLint v[4] = {x, y, w, h};
    if (memcmp(v, c->st.viewport, sizeof(v)) != 0) {
        state_changed(c);
        memcpy(c->st.viewport, v, sizeof(v));
    }
    c->viewport_init = true;
}

void impl_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glScissor(x, y, w, h));
    if (w < 0 || h < 0) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    const GLint v[4] = {x, y, w, h};
    if (memcmp(v, c->st.scissor_box, sizeof(v)) != 0) {
        state_changed(c);
        memcpy(c->st.scissor_box, v, sizeof(v));
    }
}

void impl_glClipPlane(GLenum plane, const GLdouble *equation)
{
    (void)plane;
    (void)equation; /* 设备只做视锥裁剪 */
}

void impl_glGetClipPlane(GLenum plane, GLdouble *equation)
{
    (void)plane;
    if (equation) {
        equation[0] = equation[1] = equation[2] = equation[3] = 0;
    }
}

void impl_glDrawBuffer(GLenum mode)
{
    ESPGL_CTX(c);
    c->st.draw_buffer = mode;
}

void impl_glReadBuffer(GLenum mode)
{
    ESPGL_CTX(c);
    c->st.read_buffer = mode;
}

/* ── 雾 ───────────────────────────────────────────────────────────── */

void impl_glFogfv(GLenum pname, const GLfloat *p)
{
    ESPGL_CTX(c);
    if (p == nullptr) {
        return;
    }
    if (list_recording(c)) {
        float copy[4] = {p[0], 0, 0, 0};
        if (pname == GL_FOG_COLOR) {
            memcpy(copy, p, sizeof(copy));
        }
        list_record_fn(c, [=]() { impl_glFogfv(pname, copy); });
        if (list_compile_only(c)) {
            return;
        }
    }
    State &s = c->st;
    switch (pname) {
    case GL_FOG_MODE: set_if(c, s.fog_mode, static_cast<GLenum>(p[0])); break;
    case GL_FOG_DENSITY: set_if(c, s.fog_density, p[0]); break;
    case GL_FOG_START: set_if(c, s.fog_start, p[0]); break;
    case GL_FOG_END: set_if(c, s.fog_end, p[0]); break;
    case GL_FOG_INDEX: s.fog_index = p[0]; break;
    case GL_FOG_COLOR: {
        const float v[4] = {clamp01(p[0]), clamp01(p[1]), clamp01(p[2]), clamp01(p[3])};
        set4(c, s.fog_color, v);
        break;
    }
    default: break;
    }
}

void impl_glFogf(GLenum pname, GLfloat param)
{
    impl_glFogfv(pname, &param);
}

void impl_glFogi(GLenum pname, GLint param)
{
    const GLfloat f = static_cast<GLfloat>(param);
    impl_glFogfv(pname, &f);
}

void impl_glFogiv(GLenum pname, const GLint *params)
{
    if (params == nullptr) {
        return;
    }
    GLfloat f[4];
    if (pname == GL_FOG_COLOR) {
        for (int i = 0; i < 4; i++) {
            f[i] = static_cast<GLfloat>(params[i]) / 2147483647.0f;
        }
    } else {
        f[0] = static_cast<GLfloat>(params[0]);
    }
    impl_glFogfv(pname, f);
}

/* ── 光照 / 材质 ──────────────────────────────────────────────────── */

static int param_count_light(GLenum pname)
{
    switch (pname) {
    case GL_AMBIENT:
    case GL_DIFFUSE:
    case GL_SPECULAR:
    case GL_POSITION: return 4;
    case GL_SPOT_DIRECTION: return 3;
    default: return 1;
    }
}

void impl_glLightfv(GLenum light, GLenum pname, const GLfloat *p)
{
    ESPGL_CTX(c);
    if (p == nullptr || light < GL_LIGHT0 || light >= GL_LIGHT0 + kMaxLights) {
        return;
    }
    if (list_recording(c)) {
        float copy[4] = {0, 0, 0, 0};
        memcpy(copy, p, sizeof(float) * static_cast<size_t>(param_count_light(pname)));
        list_record_fn(c, [=]() { impl_glLightfv(light, pname, copy); });
        if (list_compile_only(c)) {
            return;
        }
    }
    state_changed(c);
    LightState &L = c->st.lights[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT: memcpy(L.ambient, p, 16); break;
    case GL_DIFFUSE: memcpy(L.diffuse, p, 16); break;
    case GL_SPECULAR: memcpy(L.specular, p, 16); break;
    case GL_POSITION: {
        /* GL 语义：位置在调用时被当前 modelview 变换到眼空间 */
        float in[4] = {p[0], p[1], p[2], p[3]};
        mat_xform(c->modelview.top(), in, L.position);
        break;
    }
    case GL_SPOT_DIRECTION: {
        float in[4] = {p[0], p[1], p[2], 0};
        float out[4];
        mat_xform(c->modelview.top(), in, out);
        memcpy(L.spot_dir, out, 12);
        break;
    }
    case GL_SPOT_EXPONENT: L.spot_exp = p[0]; break;
    case GL_SPOT_CUTOFF: L.spot_cutoff = p[0]; break;
    case GL_CONSTANT_ATTENUATION: L.atten[0] = p[0]; break;
    case GL_LINEAR_ATTENUATION: L.atten[1] = p[0]; break;
    case GL_QUADRATIC_ATTENUATION: L.atten[2] = p[0]; break;
    default: break;
    }
}

void impl_glLightf(GLenum light, GLenum pname, GLfloat param)
{
    impl_glLightfv(light, pname, &param);
}

void impl_glLighti(GLenum light, GLenum pname, GLint param)
{
    const GLfloat f = static_cast<GLfloat>(param);
    impl_glLightfv(light, pname, &f);
}

void impl_glLightiv(GLenum light, GLenum pname, const GLint *params)
{
    if (params == nullptr) {
        return;
    }
    GLfloat f[4] = {0, 0, 0, 0};
    const int n = param_count_light(pname);
    const bool color = pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR;
    for (int i = 0; i < n; i++) {
        f[i] = color ? static_cast<GLfloat>(params[i]) / 2147483647.0f : static_cast<GLfloat>(params[i]);
    }
    impl_glLightfv(light, pname, f);
}

void impl_glGetLightfv(GLenum light, GLenum pname, GLfloat *p)
{
    ESPGL_CTX(c);
    if (p == nullptr || light < GL_LIGHT0 || light >= GL_LIGHT0 + kMaxLights) {
        return;
    }
    const LightState &L = c->st.lights[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT: memcpy(p, L.ambient, 16); break;
    case GL_DIFFUSE: memcpy(p, L.diffuse, 16); break;
    case GL_SPECULAR: memcpy(p, L.specular, 16); break;
    case GL_POSITION: memcpy(p, L.position, 16); break;
    case GL_SPOT_DIRECTION: memcpy(p, L.spot_dir, 12); break;
    case GL_SPOT_EXPONENT: p[0] = L.spot_exp; break;
    case GL_SPOT_CUTOFF: p[0] = L.spot_cutoff; break;
    case GL_CONSTANT_ATTENUATION: p[0] = L.atten[0]; break;
    case GL_LINEAR_ATTENUATION: p[0] = L.atten[1]; break;
    case GL_QUADRATIC_ATTENUATION: p[0] = L.atten[2]; break;
    default: break;
    }
}

void impl_glGetLightiv(GLenum light, GLenum pname, GLint *params)
{
    GLfloat f[4] = {0, 0, 0, 0};
    impl_glGetLightfv(light, pname, f);
    if (params) {
        for (int i = 0; i < param_count_light(pname); i++) {
            params[i] = static_cast<GLint>(f[i]);
        }
    }
}

void impl_glLightModelfv(GLenum pname, const GLfloat *p)
{
    ESPGL_CTX(c);
    if (p == nullptr) {
        return;
    }
    if (list_recording(c)) {
        float copy[4] = {p[0], 0, 0, 0};
        if (pname == GL_LIGHT_MODEL_AMBIENT) {
            memcpy(copy, p, 16);
        }
        list_record_fn(c, [=]() { impl_glLightModelfv(pname, copy); });
        if (list_compile_only(c)) {
            return;
        }
    }
    state_changed(c);
    switch (pname) {
    case GL_LIGHT_MODEL_AMBIENT: memcpy(c->st.light_model_ambient, p, 16); break;
    case GL_LIGHT_MODEL_TWO_SIDE: c->st.light_model_two_side = p[0] != 0; break;
    case GL_LIGHT_MODEL_LOCAL_VIEWER: c->st.light_model_local_viewer = p[0] != 0; break;
    default: break;
    }
}

void impl_glLightModelf(GLenum pname, GLfloat param)
{
    impl_glLightModelfv(pname, &param);
}

void impl_glLightModeli(GLenum pname, GLint param)
{
    const GLfloat f = static_cast<GLfloat>(param);
    impl_glLightModelfv(pname, &f);
}

void impl_glLightModeliv(GLenum pname, const GLint *params)
{
    if (params == nullptr) {
        return;
    }
    GLfloat f[4] = {static_cast<GLfloat>(params[0]), 0, 0, 0};
    if (pname == GL_LIGHT_MODEL_AMBIENT) {
        for (int i = 0; i < 4; i++) {
            f[i] = static_cast<GLfloat>(params[i]) / 2147483647.0f;
        }
    }
    impl_glLightModelfv(pname, f);
}

void impl_glMaterialfv(GLenum face, GLenum pname, const GLfloat *p)
{
    ESPGL_CTX(c);
    if (p == nullptr) {
        return;
    }
    if (list_recording(c)) {
        float copy[4] = {p[0], 0, 0, 0};
        if (pname != GL_SHININESS) {
            memcpy(copy, p, 16);
        }
        list_record_fn(c, [=]() { impl_glMaterialfv(face, pname, copy); });
        if (list_compile_only(c)) {
            return;
        }
    }
    state_changed(c);
    for (int f = 0; f < 2; f++) {
        if ((f == 0 && face == GL_BACK) || (f == 1 && face == GL_FRONT)) {
            continue;
        }
        MaterialState &m = c->st.material[f];
        switch (pname) {
        case GL_AMBIENT: memcpy(m.ambient, p, 16); break;
        case GL_DIFFUSE: memcpy(m.diffuse, p, 16); break;
        case GL_AMBIENT_AND_DIFFUSE:
            memcpy(m.ambient, p, 16);
            memcpy(m.diffuse, p, 16);
            break;
        case GL_SPECULAR: memcpy(m.specular, p, 16); break;
        case GL_EMISSION: memcpy(m.emission, p, 16); break;
        case GL_SHININESS: m.shininess = p[0]; break;
        default: break;
        }
    }
}

void impl_glMaterialf(GLenum face, GLenum pname, GLfloat param)
{
    GLfloat f[4] = {param, param, param, param};
    impl_glMaterialfv(face, pname, f);
}

void impl_glMateriali(GLenum face, GLenum pname, GLint param)
{
    impl_glMaterialf(face, pname, static_cast<GLfloat>(param));
}

void impl_glMaterialiv(GLenum face, GLenum pname, const GLint *params)
{
    if (params == nullptr) {
        return;
    }
    GLfloat f[4];
    for (int i = 0; i < 4; i++) {
        f[i] = pname == GL_SHININESS ? static_cast<GLfloat>(params[0])
                                     : static_cast<GLfloat>(params[i]) / 2147483647.0f;
        if (pname == GL_SHININESS) {
            break;
        }
    }
    impl_glMaterialfv(face, pname, f);
}

void impl_glGetMaterialfv(GLenum face, GLenum pname, GLfloat *p)
{
    ESPGL_CTX(c);
    if (p == nullptr) {
        return;
    }
    const MaterialState &m = c->st.material[face == GL_BACK ? 1 : 0];
    switch (pname) {
    case GL_AMBIENT: memcpy(p, m.ambient, 16); break;
    case GL_DIFFUSE: memcpy(p, m.diffuse, 16); break;
    case GL_SPECULAR: memcpy(p, m.specular, 16); break;
    case GL_EMISSION: memcpy(p, m.emission, 16); break;
    case GL_SHININESS: p[0] = m.shininess; break;
    default: break;
    }
}

void impl_glGetMaterialiv(GLenum face, GLenum pname, GLint *params)
{
    GLfloat f[4] = {0, 0, 0, 0};
    impl_glGetMaterialfv(face, pname, f);
    if (params) {
        const int n = pname == GL_SHININESS ? 1 : 4;
        for (int i = 0; i < n; i++) {
            params[i] = static_cast<GLint>(f[i]);
        }
    }
}

void impl_glColorMaterial(GLenum face, GLenum mode)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glColorMaterial(face, mode));
    set_if(c, c->st.color_material_face, face);
    set_if(c, c->st.color_material_mode, mode);
}

/* ── glPushAttrib / glPopAttrib ───────────────────────────────────── */

void impl_glPushAttrib(GLbitfield mask)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glPushAttrib(mask));
    if (c->attrib_stack.size() >= kAttribDepth) {
        set_error(GL_STACK_OVERFLOW);
        return;
    }
    c->attrib_stack.push_back({mask, c->st});
}

void impl_glPopAttrib(void)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glPopAttrib());
    if (c->attrib_stack.empty()) {
        set_error(GL_STACK_UNDERFLOW);
        return;
    }
    state_changed(c);
    const AttribEntry e = c->attrib_stack.back();
    c->attrib_stack.pop_back();
    const GLbitfield m = e.mask;
    const State &o = e.st;
    State &s = c->st;

    if (m & GL_COLOR_BUFFER_BIT) {
        s.blend = o.blend;
        s.blend_src = o.blend_src;
        s.blend_dst = o.blend_dst;
        s.blend_eq = o.blend_eq;
        memcpy(s.blend_color, o.blend_color, 16);
        s.alpha_test = o.alpha_test;
        s.alpha_func = o.alpha_func;
        s.alpha_ref = o.alpha_ref;
        memcpy(s.color_mask, o.color_mask, sizeof(s.color_mask));
        memcpy(s.clear_color, o.clear_color, 16);
        s.dither = o.dither;
        s.logic_op = o.logic_op;
        s.logic_op_mode = o.logic_op_mode;
        s.draw_buffer = o.draw_buffer;
    }
    if (m & GL_DEPTH_BUFFER_BIT) {
        s.depth_test = o.depth_test;
        s.depth_func = o.depth_func;
        s.depth_mask = o.depth_mask;
        s.clear_depth = o.clear_depth;
    }
    if (m & GL_STENCIL_BUFFER_BIT) {
        s.stencil_test = o.stencil_test;
        s.stencil_func = o.stencil_func;
        s.stencil_ref = o.stencil_ref;
        s.stencil_mask = o.stencil_mask;
        s.stencil_writemask = o.stencil_writemask;
        s.stencil_fail = o.stencil_fail;
        s.stencil_zfail = o.stencil_zfail;
        s.stencil_zpass = o.stencil_zpass;
        s.clear_stencil = o.clear_stencil;
    }
    if (m & GL_POLYGON_BIT) {
        s.cull = o.cull;
        s.cull_face = o.cull_face;
        s.front_face = o.front_face;
        s.polygon_mode[0] = o.polygon_mode[0];
        s.polygon_mode[1] = o.polygon_mode[1];
        s.polygon_offset_fill = o.polygon_offset_fill;
        s.polygon_offset_line = o.polygon_offset_line;
        s.polygon_offset_point = o.polygon_offset_point;
        s.offset_factor = o.offset_factor;
        s.offset_units = o.offset_units;
        s.polygon_smooth = o.polygon_smooth;
        s.polygon_stipple = o.polygon_stipple;
    }
    if (m & GL_LIGHTING_BIT) {
        s.lighting = o.lighting;
        s.shade_model = o.shade_model;
        s.color_material = o.color_material;
        s.color_material_face = o.color_material_face;
        s.color_material_mode = o.color_material_mode;
        memcpy(s.light_model_ambient, o.light_model_ambient, 16);
        s.light_model_two_side = o.light_model_two_side;
        s.light_model_local_viewer = o.light_model_local_viewer;
        for (int i = 0; i < kMaxLights; i++) {
            s.lights[i] = o.lights[i];
        }
        s.material[0] = o.material[0];
        s.material[1] = o.material[1];
    }
    if (m & GL_FOG_BIT) {
        s.fog = o.fog;
        s.fog_mode = o.fog_mode;
        s.fog_density = o.fog_density;
        s.fog_start = o.fog_start;
        s.fog_end = o.fog_end;
        memcpy(s.fog_color, o.fog_color, 16);
        s.fog_index = o.fog_index;
    }
    if (m & GL_LINE_BIT) {
        s.line_width = o.line_width;
        s.line_smooth = o.line_smooth;
        s.line_stipple = o.line_stipple;
        s.line_stipple_factor = o.line_stipple_factor;
        s.line_stipple_pattern = o.line_stipple_pattern;
    }
    if (m & GL_POINT_BIT) {
        s.point_size = o.point_size;
        s.point_smooth = o.point_smooth;
    }
    if (m & GL_SCISSOR_BIT) {
        s.scissor = o.scissor;
        memcpy(s.scissor_box, o.scissor_box, sizeof(s.scissor_box));
    }
    if (m & GL_VIEWPORT_BIT) {
        memcpy(s.viewport, o.viewport, sizeof(s.viewport));
        s.depth_range[0] = o.depth_range[0];
        s.depth_range[1] = o.depth_range[1];
    }
    if (m & GL_TEXTURE_BIT) {
        for (int i = 0; i < kMaxUnits; i++) {
            s.unit[i] = o.unit[i];
        }
        s.active_unit = o.active_unit;
    }
    if (m & GL_ENABLE_BIT) {
        s.blend = o.blend;
        s.alpha_test = o.alpha_test;
        s.depth_test = o.depth_test;
        s.cull = o.cull;
        s.fog = o.fog;
        s.lighting = o.lighting;
        s.scissor = o.scissor;
        s.color_material = o.color_material;
        s.normalize = o.normalize;
        s.rescale_normal = o.rescale_normal;
        s.polygon_offset_fill = o.polygon_offset_fill;
        s.polygon_offset_line = o.polygon_offset_line;
        s.polygon_offset_point = o.polygon_offset_point;
        s.dither = o.dither;
        s.line_smooth = o.line_smooth;
        s.point_smooth = o.point_smooth;
        s.polygon_smooth = o.polygon_smooth;
        s.line_stipple = o.line_stipple;
        s.polygon_stipple = o.polygon_stipple;
        s.stencil_test = o.stencil_test;
        s.logic_op = o.logic_op;
        s.shared_palette = o.shared_palette;
        for (int i = 0; i < kMaxLights; i++) {
            s.lights[i].enabled = o.lights[i].enabled;
        }
        for (int i = 0; i < 6; i++) {
            s.clip_plane[i] = o.clip_plane[i];
        }
        for (int i = 0; i < kMaxUnits; i++) {
            s.unit[i].enable_2d = o.unit[i].enable_2d;
            s.unit[i].enable_1d = o.unit[i].enable_1d;
            memcpy(s.unit[i].texgen, o.unit[i].texgen, sizeof(s.unit[i].texgen));
        }
    }
    if (m & GL_TRANSFORM_BIT) {
        s.matrix_mode = o.matrix_mode;
        s.normalize = o.normalize;
        s.rescale_normal = o.rescale_normal;
        for (int i = 0; i < 6; i++) {
            s.clip_plane[i] = o.clip_plane[i];
        }
    }
    if (m & GL_HINT_BIT) {
        s.hint_perspective = o.hint_perspective;
        s.hint_fog = o.hint_fog;
        s.hint_line = o.hint_line;
        s.hint_point = o.hint_point;
        s.hint_polygon = o.hint_polygon;
    }
    if (m & GL_CURRENT_BIT) {
        memcpy(s.cur_color, o.cur_color, 16);
        memcpy(s.cur_normal, o.cur_normal, 12);
        memcpy(s.cur_tc, o.cur_tc, sizeof(s.cur_tc));
        memcpy(s.raster_pos, o.raster_pos, 16);
        memcpy(s.raster_color, o.raster_color, 16);
        s.raster_valid = o.raster_valid;
        s.edge_flag = o.edge_flag;
    }
    if (m & GL_PIXEL_MODE_BIT) {
        s.zoom_x = o.zoom_x;
        s.zoom_y = o.zoom_y;
        s.read_buffer = o.read_buffer;
    }
    if (m & GL_LIST_BIT) {
        s.list_base = o.list_base;
    }
}

void impl_glPushClientAttrib(GLbitfield mask)
{
    ESPGL_CTX(c);
    if (c->client_attrib_stack.size() >= kAttribDepth) {
        set_error(GL_STACK_OVERFLOW);
        return;
    }
    c->client_attrib_stack.push_back({mask, c->cs});
}

void impl_glPopClientAttrib(void)
{
    ESPGL_CTX(c);
    if (c->client_attrib_stack.empty()) {
        set_error(GL_STACK_UNDERFLOW);
        return;
    }
    const ClientAttribEntry e = c->client_attrib_stack.back();
    c->client_attrib_stack.pop_back();
    if (e.mask & GL_CLIENT_PIXEL_STORE_BIT) {
        c->cs.unpack = e.cs.unpack;
        c->cs.pack = e.cs.pack;
    }
    if (e.mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
        c->cs.vertex = e.cs.vertex;
        c->cs.normal = e.cs.normal;
        c->cs.color = e.cs.color;
        c->cs.index = e.cs.index;
        c->cs.edge = e.cs.edge;
        for (int i = 0; i < kMaxUnits; i++) {
            c->cs.tc[i] = e.cs.tc[i];
        }
        c->cs.client_active = e.cs.client_active;
    }
}

/* ── 像素存储 ─────────────────────────────────────────────────────── */

void impl_glPixelStorei(GLenum pname, GLint param)
{
    ESPGL_CTX(c);
    PixelStore &u = c->cs.unpack;
    PixelStore &p = c->cs.pack;
    switch (pname) {
    case GL_UNPACK_ALIGNMENT: u.alignment = param; break;
    case GL_UNPACK_ROW_LENGTH: u.row_length = param; break;
    case GL_UNPACK_SKIP_ROWS: u.skip_rows = param; break;
    case GL_UNPACK_SKIP_PIXELS: u.skip_pixels = param; break;
    case GL_UNPACK_SWAP_BYTES: u.swap_bytes = param != 0; break;
    case GL_UNPACK_LSB_FIRST: u.lsb_first = param != 0; break;
    case GL_PACK_ALIGNMENT: p.alignment = param; break;
    case GL_PACK_ROW_LENGTH: p.row_length = param; break;
    case GL_PACK_SKIP_ROWS: p.skip_rows = param; break;
    case GL_PACK_SKIP_PIXELS: p.skip_pixels = param; break;
    case GL_PACK_SWAP_BYTES: p.swap_bytes = param != 0; break;
    case GL_PACK_LSB_FIRST: p.lsb_first = param != 0; break;
    default: break;
    }
}

void impl_glPixelStoref(GLenum pname, GLfloat param)
{
    impl_glPixelStorei(pname, static_cast<GLint>(param));
}

void impl_glPixelTransferf(GLenum pname, GLfloat param)
{
    (void)pname;
    (void)param;
}

void impl_glPixelTransferi(GLenum pname, GLint param)
{
    (void)pname;
    (void)param;
}

void impl_glPixelZoom(GLfloat x, GLfloat y)
{
    ESPGL_CTX(c);
    c->st.zoom_x = x;
    c->st.zoom_y = y;
}

/* ── glGetString / glGetError ─────────────────────────────────────── */

const GLubyte *impl_glGetString(GLenum name)
{
    switch (name) {
    case GL_VENDOR: return reinterpret_cast<const GLubyte *>("espgpu");
    case GL_RENDERER: return reinterpret_cast<const GLubyte *>("ESP32-S3 GPU (espgpu GL 1.2 front-end)");
    case GL_VERSION: return reinterpret_cast<const GLubyte *>("1.2 espgpu");
    case GL_EXTENSIONS: return reinterpret_cast<const GLubyte *>(kExtensions);
    default: set_error(GL_INVALID_ENUM); return nullptr;
    }
}

const char *extension_string()
{
    return kExtensions;
}

GLenum impl_glGetError(void)
{
    Context *c = cur();
    if (c == nullptr) {
        return GL_NO_ERROR;
    }
    const GLenum e = c->error;
    c->error = GL_NO_ERROR;
    return e;
}

/* ── glGet* ───────────────────────────────────────────────────────── */

namespace {

/** 统一查询：返回值个数，0 = 未知 pname；is_color 用于整数转换 */
int get_values(Context *c, GLenum pname, double *v, bool *is_color)
{
    const State &s = c->st;
    *is_color = false;
    auto put4f = [&](const float *f) {
        for (int i = 0; i < 4; i++) {
            v[i] = f[i];
        }
        return 4;
    };
    auto putmat = [&](const Mat4 &m) {
        for (int i = 0; i < 16; i++) {
            v[i] = m.m[i];
        }
        return 16;
    };

    if (bool *p = cap_ptr(c, pname)) {
        v[0] = *p ? 1 : 0;
        return 1;
    }
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE: v[0] = g_sh.max_tex_dim; return 1;
    case GL_MAX_TEXTURE_UNITS: v[0] = kMaxUnits; return 1;
    case GL_MAX_LIGHTS: v[0] = kMaxLights; return 1;
    case GL_MAX_MODELVIEW_STACK_DEPTH: v[0] = kModelviewDepth; return 1;
    case GL_MAX_PROJECTION_STACK_DEPTH: v[0] = kProjectionDepth; return 1;
    case GL_MAX_TEXTURE_STACK_DEPTH: v[0] = kTextureDepth; return 1;
    case GL_MAX_ATTRIB_STACK_DEPTH: v[0] = kAttribDepth; return 1;
    case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH: v[0] = kAttribDepth; return 1;
    case GL_MAX_LIST_NESTING: v[0] = 64; return 1;
    case GL_MAX_CLIP_PLANES: v[0] = 6; return 1;
    case GL_MAX_EVAL_ORDER: v[0] = 8; return 1;
    case GL_MAX_PIXEL_MAP_TABLE: v[0] = 256; return 1;
    case GL_MAX_NAME_STACK_DEPTH: v[0] = 64; return 1;
    case 0x80E8 /* GL_MAX_ELEMENTS_VERTICES */: v[0] = 4096; return 1;
    case 0x80E9 /* GL_MAX_ELEMENTS_INDICES */: v[0] = 65535; return 1;
    case GL_MAX_VIEWPORT_DIMS: v[0] = 4096; v[1] = 4096; return 2;
    case GL_SUBPIXEL_BITS: v[0] = 4; return 1;
    case GL_RED_BITS: v[0] = 5; return 1;
    case GL_GREEN_BITS: v[0] = 6; return 1;
    case GL_BLUE_BITS: v[0] = 5; return 1;
    case GL_ALPHA_BITS: v[0] = 0; return 1;
    case GL_DEPTH_BITS: v[0] = 16; return 1;
    case GL_STENCIL_BITS: v[0] = 0; return 1;
    case GL_INDEX_BITS: v[0] = 0; return 1;
    case GL_ACCUM_RED_BITS:
    case GL_ACCUM_GREEN_BITS:
    case GL_ACCUM_BLUE_BITS:
    case GL_ACCUM_ALPHA_BITS: v[0] = 0; return 1;
    case GL_DOUBLEBUFFER: v[0] = 1; return 1;
    case GL_RGBA_MODE: v[0] = 1; return 1;
    case GL_STEREO: v[0] = 0; return 1;
    case GL_AUX_BUFFERS: v[0] = 0; return 1;
    case GL_POINT_SIZE_RANGE: v[0] = 1; v[1] = 64; return 2;
    case GL_POINT_SIZE_GRANULARITY: v[0] = 1; return 1;
    case GL_LINE_WIDTH_RANGE: v[0] = 1; v[1] = 16; return 2;
    case GL_LINE_WIDTH_GRANULARITY: v[0] = 1; return 1;
    case 0x846D /* GL_ALIASED_POINT_SIZE_RANGE */: v[0] = 1; v[1] = 64; return 2;
    case 0x846E /* GL_ALIASED_LINE_WIDTH_RANGE */: v[0] = 1; v[1] = 16; return 2;

    case GL_VIEWPORT:
        for (int i = 0; i < 4; i++) v[i] = s.viewport[i];
        return 4;
    case GL_SCISSOR_BOX:
        for (int i = 0; i < 4; i++) v[i] = s.scissor_box[i];
        return 4;
    case GL_DEPTH_RANGE: v[0] = s.depth_range[0]; v[1] = s.depth_range[1]; return 2;
    case GL_MATRIX_MODE: v[0] = s.matrix_mode; return 1;
    case GL_MODELVIEW_MATRIX: return putmat(c->modelview.top());
    case GL_PROJECTION_MATRIX: return putmat(c->projection.top());
    case GL_TEXTURE_MATRIX: return putmat(c->texmat[s.active_unit].top());
    case 0x84E3 /* GL_TRANSPOSE_MODELVIEW_MATRIX */:
    case 0x84E4 /* GL_TRANSPOSE_PROJECTION_MATRIX */:
    case 0x84E5 /* GL_TRANSPOSE_TEXTURE_MATRIX */: {
        const Mat4 &m = pname == 0x84E3 ? c->modelview.top()
                        : pname == 0x84E4 ? c->projection.top()
                                          : c->texmat[s.active_unit].top();
        for (int r = 0; r < 4; r++)
            for (int col = 0; col < 4; col++) v[r * 4 + col] = m.m[col * 4 + r];
        return 16;
    }
    case GL_MODELVIEW_STACK_DEPTH: v[0] = static_cast<double>(c->modelview.s.size()); return 1;
    case GL_PROJECTION_STACK_DEPTH: v[0] = static_cast<double>(c->projection.s.size()); return 1;
    case GL_TEXTURE_STACK_DEPTH: v[0] = static_cast<double>(c->texmat[s.active_unit].s.size()); return 1;
    case GL_ATTRIB_STACK_DEPTH: v[0] = static_cast<double>(c->attrib_stack.size()); return 1;
    case GL_CLIENT_ATTRIB_STACK_DEPTH: v[0] = static_cast<double>(c->client_attrib_stack.size()); return 1;

    case GL_TEXTURE_BINDING_2D: v[0] = s.unit[s.active_unit].bound_2d; return 1;
    case GL_TEXTURE_BINDING_1D: v[0] = s.unit[s.active_unit].bound_1d; return 1;
    case GL_ACTIVE_TEXTURE: v[0] = GL_TEXTURE0 + s.active_unit; return 1;
    case GL_CLIENT_ACTIVE_TEXTURE: v[0] = GL_TEXTURE0 + c->cs.client_active; return 1;

    case GL_BLEND_SRC: v[0] = s.blend_src; return 1;
    case GL_BLEND_DST: v[0] = s.blend_dst; return 1;
    case 0x8009 /* GL_BLEND_EQUATION */: v[0] = s.blend_eq; return 1;
    case 0x8005 /* GL_BLEND_COLOR */: *is_color = true; return put4f(s.blend_color);
    case GL_ALPHA_TEST_FUNC: v[0] = s.alpha_func; return 1;
    case GL_ALPHA_TEST_REF: v[0] = s.alpha_ref; return 1;
    case GL_DEPTH_FUNC: v[0] = s.depth_func; return 1;
    case GL_DEPTH_WRITEMASK: v[0] = s.depth_mask ? 1 : 0; return 1;
    case GL_DEPTH_CLEAR_VALUE: v[0] = s.clear_depth; return 1;
    case GL_COLOR_WRITEMASK:
        for (int i = 0; i < 4; i++) v[i] = s.color_mask[i] ? 1 : 0;
        return 4;
    case GL_COLOR_CLEAR_VALUE: *is_color = true; return put4f(s.clear_color);
    case GL_CULL_FACE_MODE: v[0] = s.cull_face; return 1;
    case GL_FRONT_FACE: v[0] = s.front_face; return 1;
    case GL_SHADE_MODEL: v[0] = s.shade_model; return 1;
    case GL_POLYGON_MODE: v[0] = s.polygon_mode[0]; v[1] = s.polygon_mode[1]; return 2;
    case GL_POLYGON_OFFSET_FACTOR: v[0] = s.offset_factor; return 1;
    case GL_POLYGON_OFFSET_UNITS: v[0] = s.offset_units; return 1;
    case GL_LINE_WIDTH: v[0] = s.line_width; return 1;
    case GL_POINT_SIZE: v[0] = s.point_size; return 1;
    case GL_FOG_MODE: v[0] = s.fog_mode; return 1;
    case GL_FOG_DENSITY: v[0] = s.fog_density; return 1;
    case GL_FOG_START: v[0] = s.fog_start; return 1;
    case GL_FOG_END: v[0] = s.fog_end; return 1;
    case GL_FOG_COLOR: *is_color = true; return put4f(s.fog_color);
    case GL_FOG_HINT: v[0] = s.hint_fog; return 1;
    case GL_PERSPECTIVE_CORRECTION_HINT: v[0] = s.hint_perspective; return 1;
    case GL_LIGHT_MODEL_AMBIENT: *is_color = true; return put4f(s.light_model_ambient);
    case GL_LIGHT_MODEL_TWO_SIDE: v[0] = s.light_model_two_side; return 1;
    case GL_COLOR_MATERIAL_FACE: v[0] = s.color_material_face; return 1;
    case GL_COLOR_MATERIAL_PARAMETER: v[0] = s.color_material_mode; return 1;
    case GL_CURRENT_COLOR: *is_color = true; return put4f(s.cur_color);
    case GL_CURRENT_NORMAL: v[0] = s.cur_normal[0]; v[1] = s.cur_normal[1]; v[2] = s.cur_normal[2]; return 3;
    case GL_CURRENT_TEXTURE_COORDS: return put4f(s.cur_tc[s.active_unit]);
    case GL_CURRENT_RASTER_POSITION: return put4f(s.raster_pos);
    case GL_CURRENT_RASTER_COLOR: *is_color = true; return put4f(s.raster_color);
    case GL_CURRENT_RASTER_POSITION_VALID: v[0] = s.raster_valid; return 1;
    case GL_LIST_BASE: v[0] = s.list_base; return 1;
    case GL_LIST_INDEX: v[0] = c->rec ? c->rec->name : 0; return 1;
    case GL_LIST_MODE: v[0] = c->rec ? c->rec->mode : 0; return 1;
    case GL_DRAW_BUFFER: v[0] = s.draw_buffer; return 1;
    case GL_READ_BUFFER: v[0] = s.read_buffer; return 1;
    case GL_ZOOM_X: v[0] = s.zoom_x; return 1;
    case GL_ZOOM_Y: v[0] = s.zoom_y; return 1;
    case GL_STENCIL_FUNC: v[0] = s.stencil_func; return 1;
    case GL_STENCIL_REF: v[0] = s.stencil_ref; return 1;
    case GL_STENCIL_VALUE_MASK: v[0] = s.stencil_mask; return 1;
    case GL_STENCIL_WRITEMASK: v[0] = s.stencil_writemask; return 1;
    case GL_STENCIL_CLEAR_VALUE: v[0] = s.clear_stencil; return 1;
    case GL_LOGIC_OP_MODE: v[0] = s.logic_op_mode; return 1;
    case GL_RENDER_MODE: v[0] = GL_RENDER; return 1;

    case GL_UNPACK_ALIGNMENT: v[0] = c->cs.unpack.alignment; return 1;
    case GL_UNPACK_ROW_LENGTH: v[0] = c->cs.unpack.row_length; return 1;
    case GL_UNPACK_SKIP_ROWS: v[0] = c->cs.unpack.skip_rows; return 1;
    case GL_UNPACK_SKIP_PIXELS: v[0] = c->cs.unpack.skip_pixels; return 1;
    case GL_PACK_ALIGNMENT: v[0] = c->cs.pack.alignment; return 1;
    case GL_PACK_ROW_LENGTH: v[0] = c->cs.pack.row_length; return 1;
    case GL_PACK_SKIP_ROWS: v[0] = c->cs.pack.skip_rows; return 1;
    case GL_PACK_SKIP_PIXELS: v[0] = c->cs.pack.skip_pixels; return 1;

    case GL_VERTEX_ARRAY_SIZE: v[0] = c->cs.vertex.size; return 1;
    case GL_VERTEX_ARRAY_TYPE: v[0] = c->cs.vertex.type; return 1;
    case GL_VERTEX_ARRAY_STRIDE: v[0] = c->cs.vertex.stride; return 1;
    case GL_COLOR_ARRAY_SIZE: v[0] = c->cs.color.size; return 1;
    case GL_COLOR_ARRAY_TYPE: v[0] = c->cs.color.type; return 1;
    case GL_COLOR_ARRAY_STRIDE: v[0] = c->cs.color.stride; return 1;
    case GL_NORMAL_ARRAY_TYPE: v[0] = c->cs.normal.type; return 1;
    case GL_NORMAL_ARRAY_STRIDE: v[0] = c->cs.normal.stride; return 1;
    case GL_TEXTURE_COORD_ARRAY_SIZE: v[0] = c->cs.tc[c->cs.client_active].size; return 1;
    case GL_TEXTURE_COORD_ARRAY_TYPE: v[0] = c->cs.tc[c->cs.client_active].type; return 1;
    case GL_TEXTURE_COORD_ARRAY_STRIDE: v[0] = c->cs.tc[c->cs.client_active].stride; return 1;
    case GL_VERTEX_ARRAY: v[0] = c->cs.vertex.enabled; return 1;
    case GL_NORMAL_ARRAY: v[0] = c->cs.normal.enabled; return 1;
    case GL_COLOR_ARRAY: v[0] = c->cs.color.enabled; return 1;
    case GL_TEXTURE_COORD_ARRAY: v[0] = c->cs.tc[c->cs.client_active].enabled; return 1;
    default: break;
    }
    return 0;
}

} // namespace

void impl_glGetDoublev(GLenum pname, GLdouble *params)
{
    ESPGL_CTX(c);
    if (params == nullptr) {
        return;
    }
    double v[16];
    bool color;
    const int n = get_values(c, pname, v, &color);
    if (n == 0) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    for (int i = 0; i < n; i++) {
        params[i] = v[i];
    }
}

void impl_glGetFloatv(GLenum pname, GLfloat *params)
{
    ESPGL_CTX(c);
    if (params == nullptr) {
        return;
    }
    double v[16];
    bool color;
    const int n = get_values(c, pname, v, &color);
    if (n == 0) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    for (int i = 0; i < n; i++) {
        params[i] = static_cast<GLfloat>(v[i]);
    }
}

void impl_glGetIntegerv(GLenum pname, GLint *params)
{
    ESPGL_CTX(c);
    if (params == nullptr) {
        return;
    }
    double v[16];
    bool color;
    const int n = get_values(c, pname, v, &color);
    if (n == 0) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (color) {
            const double d = v[i] < -1 ? -1 : (v[i] > 1 ? 1 : v[i]);
            params[i] = static_cast<GLint>(d * 2147483647.0);
        } else {
            params[i] = static_cast<GLint>(std::lround(v[i]));
        }
    }
}

void impl_glGetBooleanv(GLenum pname, GLboolean *params)
{
    ESPGL_CTX(c);
    if (params == nullptr) {
        return;
    }
    double v[16];
    bool color;
    const int n = get_values(c, pname, v, &color);
    if (n == 0) {
        set_error(GL_INVALID_ENUM);
        return;
    }
    for (int i = 0; i < n; i++) {
        params[i] = v[i] != 0.0 ? 1 : 0;
    }
}

void impl_glGetPointerv(GLenum pname, GLvoid **params)
{
    ESPGL_CTX(c);
    if (params == nullptr) {
        return;
    }
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: *params = const_cast<void *>(c->cs.vertex.ptr); break;
    case GL_NORMAL_ARRAY_POINTER: *params = const_cast<void *>(c->cs.normal.ptr); break;
    case GL_COLOR_ARRAY_POINTER: *params = const_cast<void *>(c->cs.color.ptr); break;
    case GL_INDEX_ARRAY_POINTER: *params = const_cast<void *>(c->cs.index.ptr); break;
    case GL_EDGE_FLAG_ARRAY_POINTER: *params = const_cast<void *>(c->cs.edge.ptr); break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: *params = const_cast<void *>(c->cs.tc[c->cs.client_active].ptr); break;
    default: *params = nullptr; set_error(GL_INVALID_ENUM); break;
    }
}

/* ── 选择 / 反馈 / 求值器：不支持，给出无害结果 ─────────────────── */

GLint impl_glRenderMode(GLenum mode)
{
    (void)mode;
    return 0;
}

void impl_glInitNames(void) {}
void impl_glLoadName(GLuint name) { (void)name; }
void impl_glPushName(GLuint name) { (void)name; }
void impl_glPopName(void) {}
void impl_glPassThrough(GLfloat token) { (void)token; }

void impl_glSelectBuffer(GLsizei size, GLuint *buffer)
{
    (void)size;
    (void)buffer;
}

void impl_glFeedbackBuffer(GLsizei size, GLenum type, GLfloat *buffer)
{
    (void)size;
    (void)type;
    (void)buffer;
}

void impl_glEdgeFlag(GLboolean flag)
{
    ESPGL_CTX(c);
    c->st.edge_flag = flag != 0;
}

void impl_glEdgeFlagv(const GLboolean *flag)
{
    if (flag) {
        impl_glEdgeFlag(*flag);
    }
}

void impl_glIndexMask(GLuint mask) { (void)mask; }
void impl_glIndexd(GLdouble v) { (void)v; }
void impl_glIndexdv(const GLdouble *v) { (void)v; }
void impl_glIndexf(GLfloat v) { (void)v; }
void impl_glIndexfv(const GLfloat *v) { (void)v; }
void impl_glIndexi(GLint v) { (void)v; }
void impl_glIndexiv(const GLint *v) { (void)v; }
void impl_glIndexs(GLshort v) { (void)v; }
void impl_glIndexsv(const GLshort *v) { (void)v; }
void impl_glIndexub(GLubyte v) { (void)v; }
void impl_glIndexubv(const GLubyte *v) { (void)v; }

} // namespace espgl
