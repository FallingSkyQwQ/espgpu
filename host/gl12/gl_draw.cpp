/*
 * gl_draw.cpp — 把待发批次同步成设备命令
 *
 *   状态折叠：GL 的 blend / depth / alpha / cull / fog / texenv → espgpu_pipeline_t
 *   纹理：两层以内的 texenv 组合折成 ESPGPU_SHADE_*（见 espgpu_proto.h 的语义表）
 *   顶点压缩（PLAN §2 带宽账本）：
 *     位置   I16×3，减去本批次包围盒中心，小数位按范围自适应；中心平移折进 MVP
 *     纹理   I16×2，小数位按 |st| 最大值自适应；纹理矩阵在主机上先乘掉
 *     颜色   全批次相同 → 不带属性，发 COLOR 命令；alpha 全为 1 → RGB565；否则 RGBA8
 *     法线   仅开光照时带，I8×3 归一化
 *   范围放不下时退回 F32。
 */

#include "gl_context.h"

#include <algorithm>
#include <cmath>

namespace espgl {

namespace {

inline uint16_t to565(const float *c)
{
    auto q = [](float v, int maxv) {
        int i = static_cast<int>(std::lround((v < 0 ? 0 : v > 1 ? 1 : v) * maxv));
        return i;
    };
    return static_cast<uint16_t>((q(c[0], 31) << 11) | (q(c[1], 63) << 5) | q(c[2], 31));
}

inline uint8_t to8(float v)
{
    return static_cast<uint8_t>(std::lround((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f));
}

uint8_t map_blend(GLenum f)
{
    switch (f) {
    case GL_ZERO: return ESPGPU_BLEND_ZERO;
    case GL_ONE: return ESPGPU_BLEND_ONE;
    case GL_SRC_COLOR: return ESPGPU_BLEND_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return ESPGPU_BLEND_ONE_MINUS_SRC_COLOR;
    case GL_DST_COLOR: return ESPGPU_BLEND_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return ESPGPU_BLEND_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA: return ESPGPU_BLEND_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return ESPGPU_BLEND_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return ESPGPU_BLEND_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return ESPGPU_BLEND_ONE_MINUS_DST_ALPHA;
    case GL_SRC_ALPHA_SATURATE: return ESPGPU_BLEND_ONE;
    case 0x8001 /* GL_CONSTANT_COLOR */:
    case 0x8003 /* GL_CONSTANT_ALPHA */: return ESPGPU_BLEND_ONE;
    case 0x8002 /* GL_ONE_MINUS_CONSTANT_COLOR */:
    case 0x8004 /* GL_ONE_MINUS_CONSTANT_ALPHA */: return ESPGPU_BLEND_ZERO;
    default: return ESPGPU_BLEND_ONE;
    }
}

uint8_t map_blend_op(GLenum e)
{
    switch (e) {
    case 0x800A: return ESPGPU_BLEND_OP_SUB;
    case 0x800B: return ESPGPU_BLEND_OP_REV_SUB;
    case 0x8007: return ESPGPU_BLEND_OP_MIN;
    case 0x8008: return ESPGPU_BLEND_OP_MAX;
    default: return ESPGPU_BLEND_OP_ADD;
    }
}

uint8_t map_cmp(GLenum f)
{
    if (f >= GL_NEVER && f <= GL_ALWAYS) {
        return static_cast<uint8_t>(f - GL_NEVER); /* 顺序与 ESPGPU_CMP_* 一致 */
    }
    return ESPGPU_CMP_ALWAYS;
}

enum EnvKind { ENV_REPLACE, ENV_MODULATE, ENV_ADD, ENV_DECAL, ENV_PASS };

constexpr GLenum kGL_TEXTURE = GL_TEXTURE;
constexpr GLenum kGL_INTERPOLATE = 0x8575;
constexpr GLenum kGL_ADD_SIGNED = 0x8574;
constexpr GLenum kGL_SUBTRACT = 0x84E7;

bool is_tex_src(GLenum s)
{
    return s == kGL_TEXTURE || (s >= GL_TEXTURE0 && s < GL_TEXTURE0 + 32);
}

/** texenv（含 ARB_texture_env_combine）折成最接近的基本模式 */
EnvKind env_kind(const TexUnitState &u)
{
    switch (u.env_mode) {
    case GL_REPLACE: return ENV_REPLACE;
    case GL_MODULATE: return ENV_MODULATE;
    case GL_ADD: return ENV_ADD;
    case GL_DECAL: return ENV_DECAL;
    case GL_BLEND: return ENV_MODULATE;
    case GL_COMBINE: break;
    default: return ENV_MODULATE;
    }
    switch (u.combine_rgb) {
    case GL_REPLACE:
        return is_tex_src(u.src_rgb[0]) ? ENV_REPLACE : ENV_PASS;
    case GL_MODULATE:
        if (!is_tex_src(u.src_rgb[0]) && !is_tex_src(u.src_rgb[1])) {
            return ENV_PASS;
        }
        return ENV_MODULATE;
    case GL_ADD:
    case kGL_ADD_SIGNED:
        return ENV_ADD;
    case kGL_INTERPOLATE:
        if (is_tex_src(u.src_rgb[0]) && u.op_rgb[2] == GL_SRC_ALPHA && is_tex_src(u.src_rgb[2])) {
            return ENV_DECAL;
        }
        return ENV_MODULATE;
    case kGL_SUBTRACT:
    default:
        return ENV_MODULATE;
    }
}

int16_t clamp16(long v)
{
    return static_cast<int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

} // namespace

/* ── 视口 / 裁剪 ──────────────────────────────────────────────────── */

void win_rect_to_dev(Context *c, GLint x, GLint y, GLint w, GLint h, int16_t out[4])
{
    const double sx = static_cast<double>(g_sh.dev_w) / std::max(1, c->win_w);
    const double sy = static_cast<double>(g_sh.dev_h) / std::max(1, c->win_h);
    const long x0 = std::lround(x * sx);
    const long x1 = std::lround((x + w) * sx);
    const long y0 = std::lround(y * sy);
    const long y1 = std::lround((y + h) * sy);
    out[0] = clamp16(x0);
    out[1] = clamp16(g_sh.dev_h - y1);
    out[2] = clamp16(x1 - x0);
    out[3] = clamp16(y1 - y0);
}

void sync_viewport_scissor(Context *c)
{
    espgpu_device_t *dev = g_sh.dev;
    if (dev == nullptr) {
        return;
    }
    if (g_sh.dc_owner != c) {
        /* 设备状态是全局的：上一次是别的上下文在用，缓存不可信 */
        c->dc = DevCache();
        g_sh.dc_owner = c;
    }
    DevCache &dc = c->dc;
    int16_t vp[4];
    win_rect_to_dev(c, c->st.viewport[0], c->st.viewport[1], c->st.viewport[2], c->st.viewport[3], vp);
    if (memcmp(vp, dc.viewport, sizeof(vp)) != 0) {
        espgpu_cmd_viewport(dev, vp[0], vp[1], vp[2], vp[3]);
        memcpy(dc.viewport, vp, sizeof(vp));
    }
    int16_t sc[4];
    if (c->st.scissor) {
        win_rect_to_dev(c, c->st.scissor_box[0], c->st.scissor_box[1], c->st.scissor_box[2],
                        c->st.scissor_box[3], sc);
        /* 裁到设备范围内（设备的裁剪矩形是无符号的） */
        int x0 = std::max<int>(0, sc[0]), y0 = std::max<int>(0, sc[1]);
        int x1 = std::min<int>(g_sh.dev_w, sc[0] + sc[2]), y1 = std::min<int>(g_sh.dev_h, sc[1] + sc[3]);
        sc[0] = static_cast<int16_t>(x0);
        sc[1] = static_cast<int16_t>(y0);
        sc[2] = static_cast<int16_t>(std::max(0, x1 - x0));
        sc[3] = static_cast<int16_t>(std::max(0, y1 - y0));
    } else {
        sc[0] = 0;
        sc[1] = 0;
        sc[2] = static_cast<int16_t>(g_sh.dev_w);
        sc[3] = static_cast<int16_t>(g_sh.dev_h);
    }
    if (memcmp(sc, dc.scissor, sizeof(sc)) != 0) {
        espgpu_cmd_scissor(dev, sc[0], sc[1], sc[2], sc[3]);
        memcpy(dc.scissor, sc, sizeof(sc));
    }
    const float dr[2] = {static_cast<float>(c->st.depth_range[0]), static_cast<float>(c->st.depth_range[1])};
    if (dr[0] != dc.depth_range[0] || dr[1] != dc.depth_range[1]) {
        espgpu_cmd_depth_range(dev, dr[0], dr[1]);
        dc.depth_range[0] = dr[0];
        dc.depth_range[1] = dr[1];
    }
}

/* ── 状态同步 ─────────────────────────────────────────────────────── */

static void sync_lighting(Context *c)
{
    const State &s = c->st;
    std::vector<uint8_t> bytes;
    espgpu_lighting_t L{};
    espgpu_light_t lights[ESPGPU_MAX_LIGHTS];
    uint32_t n = 0;
    if (s.lighting) {
        L.enable = 1;
        float amb[3] = {s.light_model_ambient[0], s.light_model_ambient[1], s.light_model_ambient[2]};
        for (int i = 0; i < kMaxLights; i++) {
            const LightState &ls = s.lights[i];
            if (!ls.enabled) {
                continue;
            }
            for (int k = 0; k < 3; k++) {
                amb[k] += ls.ambient[k];
            }
            if (n >= ESPGPU_MAX_LIGHTS) {
                continue;
            }
            /* 方向光直接用；点光源近似成从眼点指向光源的方向光 */
            float d[3] = {ls.position[0], ls.position[1], ls.position[2]};
            if (ls.position[3] != 0.0f) {
                d[0] /= ls.position[3];
                d[1] /= ls.position[3];
                d[2] /= ls.position[3];
            }
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 0) {
                d[0] /= len;
                d[1] /= len;
                d[2] /= len;
            } else {
                d[2] = 1;
            }
            espgpu_light_t &o = lights[n++];
            memcpy(o.dir, d, sizeof(o.dir));
            o.diffuse[0] = ls.diffuse[0];
            o.diffuse[1] = ls.diffuse[1];
            o.diffuse[2] = ls.diffuse[2];
        }
        /* 没开 COLOR_MATERIAL 时设备的顶点色就是材质漫反射，环境项也乘它，误差可接受 */
        L.ambient[0] = std::min(1.0f, amb[0] + s.material[0].emission[0]);
        L.ambient[1] = std::min(1.0f, amb[1] + s.material[0].emission[1]);
        L.ambient[2] = std::min(1.0f, amb[2] + s.material[0].emission[2]);
    }
    L.count = static_cast<uint8_t>(n);
    bytes.resize(sizeof(L) + n * sizeof(espgpu_light_t));
    memcpy(bytes.data(), &L, sizeof(L));
    if (n) {
        memcpy(bytes.data() + sizeof(L), lights, n * sizeof(espgpu_light_t));
    }
    if (bytes != c->dc.lighting) {
        espgpu_cmd_lighting(g_sh.dev, L.enable != 0, L.ambient, lights, n);
        c->dc.lighting = std::move(bytes);
    }

    if (s.lighting) {
        Mat4 inv;
        if (!mat_inverse(c->modelview.top(), inv)) {
            inv = Mat4::identity();
        }
        /* N = inv(MV)^T 的行主序恰好是 inv 的列主序数组（只取 3x3） */
        float nm[16] = {};
        for (int r = 0; r < 3; r++) {
            for (int col = 0; col < 3; col++) {
                nm[r * 4 + col] = static_cast<float>(inv.m[r * 4 + col]);
            }
        }
        nm[15] = 1;
        if (!c->dc.normal_valid || memcmp(nm, c->dc.normal_m, sizeof(nm)) != 0) {
            espgpu_cmd_matrix(g_sh.dev, ESPGPU_MATRIX_NORMAL, nm);
            memcpy(c->dc.normal_m, nm, sizeof(nm));
            c->dc.normal_valid = true;
        }
    }
}

bool sync_device_state(Context *c, int *shade_out, bool *force_white, int dev_units[2], int *n_units)
{
    espgpu_device_t *dev = g_sh.dev;
    if (dev == nullptr) {
        return false;
    }
    device_poll_events();
    sync_viewport_scissor(c);

    const State &s = c->st;

    /* 纹理层 */
    int nu = 0;
    EnvKind kinds[2] = {ENV_MODULATE, ENV_MODULATE};
    for (int u = 0; u < kMaxUnits; u++) {
        const TexUnitState &us = s.unit[u];
        if (!us.enable_2d && !us.enable_1d) {
            continue;
        }
        Texture *t = tex_bound(c, u);
        if (!tex_usable(t)) {
            continue;
        }
        const EnvKind k = env_kind(us);
        if (k == ENV_PASS) {
            continue;
        }
        kinds[nu] = k;
        dev_units[nu++] = u;
    }

    int shade = ESPGPU_SHADE_FLAT;
    bool white = false;
    if (nu == 1) {
        switch (kinds[0]) {
        case ENV_REPLACE: shade = ESPGPU_SHADE_TEX; break;
        case ENV_ADD: shade = ESPGPU_SHADE_TEX_ADD; break;
        case ENV_DECAL: shade = ESPGPU_SHADE_TEX_DECAL; break;
        default: shade = ESPGPU_SHADE_TEX_MOD_COLOR; break;
        }
    } else if (nu == 2) {
        if (kinds[1] == ENV_ADD) {
            shade = ESPGPU_SHADE_TEX_ADD_TEX;
        } else {
            shade = ESPGPU_SHADE_TEX_MOD_TEX;
        }
        white = kinds[0] == ENV_REPLACE;
    }

    DevCache &dcb = c->dc;
    if (dcb.bind_epoch != g_sh.bind_epoch) {
        dcb.bound_valid = false;
    }
    /* 纹理驻留 + 绑定：先给本次要用的纹理打上本帧戳，换出时不会选中它们 */
    Texture *tex[2] = {nullptr, nullptr};
    for (int k = 0; k < nu; k++) {
        tex[k] = tex_bound(c, dev_units[k]);
        tex[k]->last_used = g_sh.frame;
        tex[k]->pin = true;
    }
    for (int k = 0; k < nu; k++) {
        const uint32_t e0 = g_sh.bind_epoch;
        if (!tex_make_resident(tex[k])) {
            /* 放不进缓存：退化为少一层纹理，至少不画错 */
            nu = k;
            shade = nu == 0 ? ESPGPU_SHADE_FLAT : ESPGPU_SHADE_TEX_MOD_COLOR;
            break;
        }
        if (g_sh.bind_epoch != e0) {
            dcb.bound_valid = false; /* 设备上的纹理槽变过，已有绑定不可信 */
        }
        if (!dcb.bound_valid) {
            for (int j = k; j < static_cast<int>(ESPGPU_MAX_TEXTURE_UNITS); j++) {
                dcb.bound[j] = 0;
            }
            dcb.bound_valid = true;
        }
        const uint16_t id = tex[k]->dev_id;
        const uint8_t filt = tex_dev_filter(tex[k]);
        const uint8_t wrap = tex_dev_wrap(tex[k]);
        if (dcb.bound[k] != id || dcb.bound_filter[k] != filt || dcb.bound_wrap[k] != wrap) {
            espgpu_cmd_bind_texture_uv(dev, static_cast<uint8_t>(k), id, filt,
                                       static_cast<uint8_t>(wrap & 0x0F),
                                       static_cast<uint8_t>(wrap >> 4));
            dcb.bound[k] = id;
            dcb.bound_filter[k] = filt;
            dcb.bound_wrap[k] = wrap;
        }
    }
    for (int k = 0; k < 2; k++) {
        if (tex[k]) {
            tex[k]->pin = false;
        }
    }
    if (!dcb.bound_valid) {
        memset(dcb.bound, 0, sizeof(dcb.bound));
        dcb.bound_valid = true;
    }
    dcb.bind_epoch = g_sh.bind_epoch;

    /* 管线 */
    espgpu_pipeline_t p{};
    p.mask = static_cast<uint16_t>(0xFFFF & ~ESPGPU_PS_TEXTURE & ~ESPGPU_PS_SCISSOR);
    p.blend_enable = s.blend ? 1 : 0;
    p.blend_src = map_blend(s.blend_src);
    p.blend_dst = map_blend(s.blend_dst);
    p.blend_op = map_blend_op(s.blend_eq);
    p.depth_test = s.depth_test ? 1 : 0;
    p.depth_write = (s.depth_test && s.depth_mask) ? 1 : 0;
    p.depth_func = s.depth_test ? map_cmp(s.depth_func) : ESPGPU_CMP_ALWAYS;
    p.alpha_test = s.alpha_test ? 1 : 0;
    p.alpha_func = map_cmp(s.alpha_func);
    p.alpha_ref = to8(s.alpha_ref);
    if (s.cull) {
        p.cull_mode = s.cull_face == GL_FRONT ? ESPGPU_CULL_FRONT
                      : s.cull_face == GL_FRONT_AND_BACK ? ESPGPU_CULL_BOTH
                                                         : ESPGPU_CULL_BACK;
    } else {
        p.cull_mode = ESPGPU_CULL_NONE;
    }
    p.front_face = s.front_face == GL_CW ? ESPGPU_FRONT_CW : ESPGPU_FRONT_CCW;
    p.shade_mode = static_cast<uint8_t>(shade);
    p.fog_enable = s.fog ? 1 : 0;
    p.fog_mode = s.fog_mode == GL_LINEAR ? ESPGPU_FOG_LINEAR
                 : s.fog_mode == GL_EXP2 ? ESPGPU_FOG_EXP2
                                         : ESPGPU_FOG_EXP;
    p.fog_color = to565(s.fog_color);

    espgpu_pipeline_fog_t fog{};
    fog.fog_start = s.fog_start;
    fog.fog_end = s.fog_end;
    fog.fog_density = s.fog_density;

    espgpu_pipeline_ext_t ext{};
    ext.shade_model = s.shade_model == GL_FLAT ? ESPGPU_SHADE_MODEL_FLAT : ESPGPU_SHADE_MODEL_SMOOTH;
    ext.color_write = (s.color_mask[0] || s.color_mask[1] || s.color_mask[2]) ? 1 : 0;
    ext.lighting = s.lighting ? 1 : 0;
    if (s.polygon_offset_fill) {
        /* 只能表达「往近处拉」：GL 的负偏移。16 位深度，units 直接用，factor 粗估斜率 4 */
        const float o = -(s.offset_units + s.offset_factor * 4.0f);
        ext.depth_bias = static_cast<uint8_t>(o <= 0 ? 0 : (o > 255 ? 255 : std::lround(o)));
    }
    const double sy = static_cast<double>(g_sh.dev_h) / std::max(1, c->win_h);
    ext.line_width = static_cast<uint8_t>(std::clamp<long>(std::lround(s.line_width * sy), 1, 255));
    ext.point_size = static_cast<uint8_t>(std::clamp<long>(std::lround(s.point_size * sy), 1, 255));

    DevCache &dc = c->dc;
    if (!dc.valid || memcmp(&p, &dc.pipe, sizeof(p)) != 0 || memcmp(&fog, &dc.fog, sizeof(fog)) != 0 ||
        memcmp(&ext, &dc.ext, sizeof(ext)) != 0) {
        espgpu_cmd_pipeline(dev, &p, &fog, &ext);
        dc.pipe = p;
        dc.fog = fog;
        dc.ext = ext;
        dc.valid = true;
    }

    sync_lighting(c);

    *shade_out = shade;
    *force_white = white;
    *n_units = nu;
    return true;
}

/* ── 顶点编码 ─────────────────────────────────────────────────────── */

namespace {

struct Plan {
    espgpu_vertex_layout_t L{};
    double origin[3] = {0, 0, 0};
    bool pos_f32 = false;
    int pos_comps = 3;
    int pos_frac = 0;
    int ntc = 0;
    int tc_gl_unit[2] = {0, 0};
    bool tc_f32[2] = {false, false};
    int tc_frac[2] = {0, 0};
    int color_mode = 0; /* 0 常量 1 RGB565 2 RGBA8 */
    uint8_t const_color[4] = {255, 255, 255, 255};
    bool normal = false;
    uint16_t off_pos = 0, off_tc[2] = {0, 0}, off_color = 0, off_normal = 0, stride = 0;
};

int frac_for(double extent)
{
    if (extent <= 0) {
        return 15;
    }
    int f = 0;
    while (f < 15 && extent * std::ldexp(1.0, f + 1) <= 32767.0) {
        f++;
    }
    return f;
}

uint16_t align_up(uint16_t v, uint16_t a)
{
    return static_cast<uint16_t>((v + a - 1) & ~(a - 1));
}

void add_attr(Plan &P, uint8_t slot, uint8_t type, uint8_t comps, uint8_t norm, uint16_t off, uint8_t frac)
{
    espgpu_vertex_attrib_t &a = P.L.attribs[P.L.attrib_count++];
    a.slot = slot;
    a.type = type;
    a.components = comps;
    a.normalized = norm;
    a.offset = off;
    a.frac_bits = frac;
    a.pad = 0;
}

/** 预处理：最终顶点色、纹理矩阵 */
void preprocess(Context *c, std::vector<Vertex> &v, bool force_white, const Plan &P)
{
    const State &s = c->st;
    const bool lit_const = s.lighting && !(s.color_material && (s.color_material_mode == GL_DIFFUSE ||
                                                                 s.color_material_mode == GL_AMBIENT_AND_DIFFUSE));
    bool texm[2] = {false, false};
    for (int k = 0; k < P.ntc; k++) {
        texm[k] = !c->texmat[P.tc_gl_unit[k]].top().is_identity();
    }
    for (Vertex &x : v) {
        if (force_white) {
            x.color[0] = x.color[1] = x.color[2] = x.color[3] = 1.0f;
        } else if (lit_const) {
            memcpy(x.color, s.material[0].diffuse, sizeof(x.color));
        }
        for (int k = 0; k < P.ntc; k++) {
            if (!texm[k]) {
                continue;
            }
            const int u = P.tc_gl_unit[k];
            float out[4];
            mat_xform(c->texmat[u].top(), x.tc[u], out);
            const float q = out[3] != 0.0f ? out[3] : 1.0f;
            x.tc[u][0] = out[0] / q;
            x.tc[u][1] = out[1] / q;
        }
        if (s.lighting) {
            const float l = std::sqrt(x.normal[0] * x.normal[0] + x.normal[1] * x.normal[1] +
                                      x.normal[2] * x.normal[2]);
            if (l > 0) {
                x.normal[0] /= l;
                x.normal[1] /= l;
                x.normal[2] /= l;
            }
        }
    }
}

void plan_build(Context *c, std::vector<Vertex> *lists[], int nlists, int dev_units[2], int nu, Plan &P)
{
    /* 位置 */
    double mn[3] = {1e300, 1e300, 1e300}, mx[3] = {-1e300, -1e300, -1e300};
    bool w_not_one = false;
    bool first = true;
    const float *c0 = nullptr;
    bool color_const = true, alpha_one = true;
    double tmax[2] = {0, 0};
    for (int l = 0; l < nlists; l++) {
        for (const Vertex &v : *lists[l]) {
            if (v.pos[3] != 1.0f) {
                w_not_one = true;
            }
            for (int k = 0; k < 3; k++) {
                mn[k] = std::min(mn[k], static_cast<double>(v.pos[k]));
                mx[k] = std::max(mx[k], static_cast<double>(v.pos[k]));
            }
            if (first) {
                c0 = v.color;
                first = false;
            } else if (color_const && memcmp(c0, v.color, sizeof(v.color)) != 0) {
                color_const = false;
            }
            if (v.color[3] < 0.999f) {
                alpha_one = false;
            }
            for (int k = 0; k < nu; k++) {
                const float *t = v.tc[dev_units[k]];
                tmax[k] = std::max(tmax[k], static_cast<double>(std::fabs(t[0])));
                tmax[k] = std::max(tmax[k], static_cast<double>(std::fabs(t[1])));
            }
        }
    }

    P.L = espgpu_vertex_layout_t{};
    P.L.index_type = 1;
    uint16_t off = 0;

    if (w_not_one) {
        P.pos_f32 = true;
        P.pos_comps = 4;
    } else {
        double ext = 0;
        for (int k = 0; k < 3; k++) {
            P.origin[k] = (mn[k] + mx[k]) * 0.5;
            ext = std::max(ext, (mx[k] - mn[k]) * 0.5);
        }
        if (ext * 1.0 > 32767.0 || !std::isfinite(ext)) {
            P.pos_f32 = true;
            P.pos_comps = 3;
            P.origin[0] = P.origin[1] = P.origin[2] = 0;
        } else {
            P.pos_frac = frac_for(ext);
        }
    }
    if (P.pos_f32) {
        P.off_pos = off;
        add_attr(P, ESPGPU_ATTR_POSITION, ESPGPU_TYPE_F32, static_cast<uint8_t>(P.pos_comps), 0, off, 0);
        off = static_cast<uint16_t>(off + 4 * P.pos_comps);
    } else {
        P.off_pos = off;
        add_attr(P, ESPGPU_ATTR_POSITION, ESPGPU_TYPE_I16, 3, 0, off, static_cast<uint8_t>(P.pos_frac));
        off = static_cast<uint16_t>(off + 6);
    }

    P.ntc = nu;
    for (int k = 0; k < nu; k++) {
        P.tc_gl_unit[k] = dev_units[k];
        if (tmax[k] >= 32767.0 || !std::isfinite(tmax[k])) {
            P.tc_f32[k] = true;
            off = align_up(off, 4);
            P.off_tc[k] = off;
            add_attr(P, static_cast<uint8_t>(ESPGPU_ATTR_TEXCOORD0 + k), ESPGPU_TYPE_F32, 2, 0, off, 0);
            off = static_cast<uint16_t>(off + 8);
        } else {
            P.tc_frac[k] = frac_for(tmax[k]);
            off = align_up(off, 2);
            P.off_tc[k] = off;
            add_attr(P, static_cast<uint8_t>(ESPGPU_ATTR_TEXCOORD0 + k), ESPGPU_TYPE_I16, 2, 0, off,
                     static_cast<uint8_t>(P.tc_frac[k]));
            off = static_cast<uint16_t>(off + 4);
        }
    }

    if (color_const && c0) {
        P.color_mode = 0;
        for (int k = 0; k < 4; k++) {
            P.const_color[k] = to8(c0[k]);
        }
    } else if (alpha_one) {
        P.color_mode = 1;
        off = align_up(off, 2);
        P.off_color = off;
        add_attr(P, ESPGPU_ATTR_COLOR, ESPGPU_TYPE_RGB565, 1, 0, off, 0);
        off = static_cast<uint16_t>(off + 2);
    } else {
        P.color_mode = 2;
        P.off_color = off;
        add_attr(P, ESPGPU_ATTR_COLOR, ESPGPU_TYPE_U8, 4, 1, off, 0);
        off = static_cast<uint16_t>(off + 4);
    }

    if (c->st.lighting) {
        P.normal = true;
        P.off_normal = off;
        add_attr(P, ESPGPU_ATTR_NORMAL, ESPGPU_TYPE_I8, 3, 1, off, 0);
        off = static_cast<uint16_t>(off + 3);
    }

    const uint16_t al = (P.pos_f32 || P.tc_f32[0] || P.tc_f32[1]) ? 4 : 2;
    P.stride = align_up(off, al);
    P.L.stride = P.stride;
}

void encode(const Plan &P, const std::vector<Vertex> &v, std::vector<uint8_t> &out)
{
    out.assign(v.size() * P.stride, 0);
    uint8_t *o = out.data();
    for (const Vertex &x : v) {
        if (P.pos_f32) {
            memcpy(o + P.off_pos, x.pos, 4u * static_cast<unsigned>(P.pos_comps));
        } else {
            const double sc = std::ldexp(1.0, P.pos_frac);
            int16_t q[3];
            for (int k = 0; k < 3; k++) {
                q[k] = clamp16(std::lround((x.pos[k] - P.origin[k]) * sc));
            }
            memcpy(o + P.off_pos, q, 6);
        }
        for (int k = 0; k < P.ntc; k++) {
            const float *t = x.tc[P.tc_gl_unit[k]];
            if (P.tc_f32[k]) {
                memcpy(o + P.off_tc[k], t, 8);
            } else {
                const double sc = std::ldexp(1.0, P.tc_frac[k]);
                const int16_t q[2] = {clamp16(std::lround(t[0] * sc)), clamp16(std::lround(t[1] * sc))};
                memcpy(o + P.off_tc[k], q, 4);
            }
        }
        if (P.color_mode == 1) {
            const uint16_t c565 = to565(x.color);
            memcpy(o + P.off_color, &c565, 2);
        } else if (P.color_mode == 2) {
            const uint8_t rgba[4] = {to8(x.color[0]), to8(x.color[1]), to8(x.color[2]), to8(x.color[3])};
            memcpy(o + P.off_color, rgba, 4);
        }
        if (P.normal) {
            float l = std::sqrt(x.normal[0] * x.normal[0] + x.normal[1] * x.normal[1] + x.normal[2] * x.normal[2]);
            if (l <= 0) {
                l = 1;
            }
            for (int k = 0; k < 3; k++) {
                const long n = std::lround(x.normal[k] / l * 127.0f);
                o[P.off_normal + k] = static_cast<uint8_t>(static_cast<int8_t>(n < -127 ? -127 : (n > 127 ? 127 : n)));
            }
        }
        o += P.stride;
    }
}

/** MVP（行主序 float）= P * MV * T(origin) */
void build_mvp(Context *c, const double origin[3], float out[16])
{
    Mat4 m = mat_mul(c->projection.top(), c->modelview.top());
    if (origin[0] != 0 || origin[1] != 0 || origin[2] != 0) {
        Mat4 t = Mat4::identity();
        t.m[12] = origin[0];
        t.m[13] = origin[1];
        t.m[14] = origin[2];
        m = mat_mul(m, t);
    }
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            out[r * 4 + col] = static_cast<float>(m.m[col * 4 + r]);
        }
    }
}

} // namespace

/** 发送布局 / MVP / 常量色（与缓存比对，只发变化的） */
static void send_draw_prologue(Context *c, const espgpu_vertex_layout_t &L, const double origin[3],
                               bool const_color, const uint8_t cc[4])
{
    espgpu_device_t *dev = g_sh.dev;
    DevCache &dc = c->dc;
    if (!dc.layout_valid || memcmp(&L, &dc.layout, sizeof(L)) != 0) {
        espgpu_cmd_vertex_layout(dev, &L);
        dc.layout = L;
        dc.layout_valid = true;
    }
    float mvp[16];
    build_mvp(c, origin, mvp);
    if (!dc.mvp_valid || memcmp(mvp, dc.mvp, sizeof(mvp)) != 0) {
        espgpu_cmd_matrix(dev, ESPGPU_MATRIX_MVP, mvp);
        memcpy(dc.mvp, mvp, sizeof(mvp));
        dc.mvp_valid = true;
    }
    if (const_color && (!dc.color_valid || memcmp(cc, dc.color, 4) != 0)) {
        espgpu_cmd_color(dev, cc[0], cc[1], cc[2], cc[3]);
        memcpy(dc.color, cc, 4);
        dc.color_valid = true;
    }
}

void send_layout_and_matrix(Context *c, const espgpu_vertex_layout_t &L, const double origin[3],
                            bool const_color, const uint8_t cc[4])
{
    send_draw_prologue(c, L, origin, const_color, cc);
}

void flush_pending(Context *c)
{
    Pending &p = c->pending;
    if (p.empty()) {
        return;
    }
    if (g_sh.dev == nullptr) {
        p.clear();
        return;
    }

    int shade = 0, nu = 0;
    bool white = false;
    int dev_units[2] = {0, 0};
    if (!sync_device_state(c, &shade, &white, dev_units, &nu)) {
        p.clear();
        return;
    }

    std::vector<Vertex> *lists[4];
    int nl = 0;
    if (!p.quads.empty()) lists[nl++] = &p.quads;
    if (!p.tri_v.empty()) lists[nl++] = &p.tri_v;
    if (!p.line_v.empty()) lists[nl++] = &p.line_v;
    if (!p.points.empty()) lists[nl++] = &p.points;

    Plan P;
    P.ntc = nu;
    for (int k = 0; k < nu; k++) {
        P.tc_gl_unit[k] = dev_units[k];
    }
    for (int l = 0; l < nl; l++) {
        preprocess(c, *lists[l], white, P);
    }
    plan_build(c, lists, nl, dev_units, nu, P);
    send_draw_prologue(c, P.L, P.origin, P.color_mode == 0, P.const_color);

    espgpu_device_t *dev = g_sh.dev;
    std::vector<uint8_t> blob;
    if (!p.quads.empty()) {
        encode(P, p.quads, blob);
        espgpu_cmd_draw(dev, ESPGPU_PRIM_QUADS, blob.data(), static_cast<uint32_t>(p.quads.size()), P.stride,
                        nullptr, 0, 1);
    }
    if (!p.tri_v.empty() && !p.tri_i.empty()) {
        encode(P, p.tri_v, blob);
        espgpu_cmd_draw(dev, ESPGPU_PRIM_TRIANGLES, blob.data(), static_cast<uint32_t>(p.tri_v.size()), P.stride,
                        p.tri_i.data(), static_cast<uint32_t>(p.tri_i.size()), 1);
    }
    if (!p.line_v.empty() && !p.line_i.empty()) {
        encode(P, p.line_v, blob);
        espgpu_cmd_draw(dev, ESPGPU_PRIM_LINES, blob.data(), static_cast<uint32_t>(p.line_v.size()), P.stride,
                        p.line_i.data(), static_cast<uint32_t>(p.line_i.size()), 1);
    }
    if (!p.points.empty()) {
        encode(P, p.points, blob);
        espgpu_cmd_draw(dev, ESPGPU_PRIM_POINTS, blob.data(), static_cast<uint32_t>(p.points.size()), P.stride,
                        nullptr, 0, 1);
    }
    p.clear();
}

/* 显示列表缓存用：按列表整体规划布局并编码（gl_list.cpp） */
bool list_plan_encode(Context *c, std::vector<Vertex> &all, bool want_color, bool want_tc[2], bool want_normal,
                      espgpu_vertex_layout_t &L, double origin[3], std::vector<uint8_t> &blob)
{
    (void)c;
    double mn[3] = {1e300, 1e300, 1e300}, mx[3] = {-1e300, -1e300, -1e300};
    double tmax[2] = {0, 0};
    bool f32 = false;
    for (const Vertex &v : all) {
        if (v.pos[3] != 1.0f) {
            f32 = true;
        }
        for (int k = 0; k < 3; k++) {
            mn[k] = std::min(mn[k], static_cast<double>(v.pos[k]));
            mx[k] = std::max(mx[k], static_cast<double>(v.pos[k]));
        }
        for (int u = 0; u < kMaxUnits; u++) {
            tmax[u] = std::max({tmax[u], static_cast<double>(std::fabs(v.tc[u][0])),
                                static_cast<double>(std::fabs(v.tc[u][1]))});
        }
    }
    Plan P;
    P.L = espgpu_vertex_layout_t{};
    P.L.index_type = 1;
    uint16_t off = 0;
    double ext = 0;
    for (int k = 0; k < 3; k++) {
        P.origin[k] = (mn[k] + mx[k]) * 0.5;
        ext = std::max(ext, (mx[k] - mn[k]) * 0.5);
    }
    if (f32 || ext > 32767.0 || !std::isfinite(ext)) {
        P.pos_f32 = true;
        P.pos_comps = 4;
        P.origin[0] = P.origin[1] = P.origin[2] = 0;
        add_attr(P, ESPGPU_ATTR_POSITION, ESPGPU_TYPE_F32, 4, 0, 0, 0);
        off = 16;
    } else {
        P.pos_frac = frac_for(ext);
        add_attr(P, ESPGPU_ATTR_POSITION, ESPGPU_TYPE_I16, 3, 0, 0, static_cast<uint8_t>(P.pos_frac));
        off = 6;
    }
    /* 列表缓存里纹理坐标按 GL 单元存：设备单元 k ← GL 单元 k（回放时要求 GL 单元与设备单元一一对应） */
    for (int u = 0; u < kMaxUnits; u++) {
        if (!want_tc[u]) {
            continue;
        }
        P.tc_gl_unit[P.ntc] = u;
        const int k = P.ntc++;
        if (tmax[u] >= 32767.0) {
            P.tc_f32[k] = true;
            off = align_up(off, 4);
            P.off_tc[k] = off;
            add_attr(P, static_cast<uint8_t>(ESPGPU_ATTR_TEXCOORD0 + u), ESPGPU_TYPE_F32, 2, 0, off, 0);
            off = static_cast<uint16_t>(off + 8);
        } else {
            P.tc_frac[k] = frac_for(tmax[u]);
            off = align_up(off, 2);
            P.off_tc[k] = off;
            add_attr(P, static_cast<uint8_t>(ESPGPU_ATTR_TEXCOORD0 + u), ESPGPU_TYPE_I16, 2, 0, off,
                     static_cast<uint8_t>(P.tc_frac[k]));
            off = static_cast<uint16_t>(off + 4);
        }
    }
    if (want_color) {
        P.color_mode = 2;
        P.off_color = off;
        add_attr(P, ESPGPU_ATTR_COLOR, ESPGPU_TYPE_U8, 4, 1, off, 0);
        off = static_cast<uint16_t>(off + 4);
    }
    if (want_normal) {
        P.normal = true;
        P.off_normal = off;
        add_attr(P, ESPGPU_ATTR_NORMAL, ESPGPU_TYPE_I8, 3, 1, off, 0);
        off = static_cast<uint16_t>(off + 3);
    }
    const uint16_t al = (P.pos_f32 || P.tc_f32[0] || P.tc_f32[1]) ? 4 : 2;
    P.stride = align_up(off, al);
    P.L.stride = P.stride;
    encode(P, all, blob);
    L = P.L;
    origin[0] = P.origin[0];
    origin[1] = P.origin[1];
    origin[2] = P.origin[2];
    return !blob.empty();
}

} // namespace espgl
