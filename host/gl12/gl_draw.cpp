/*
 * gl_draw.cpp — 把待发批次同步成设备命令
 *
 *   状态折叠：GL 的 blend / depth / alpha / cull / fog / texenv → DrawKey（resolve_key，纯函数）
 *             → espgpu_pipeline_t；纹理两层以内的 texenv 组合折成 ESPGPU_SHADE_*（见 espgpu_proto.h 的语义表）
 *   批次：每个批次带着开始时的状态快照与矩阵，flush 时换上快照同步设备（见 gl_vertex.cpp 的 begin_geometry）
 *   顶点压缩（PLAN §2 带宽账本）：
 *     位置   I16×3，原点吸附 2 的幂网格、小数位按范围自适应；原点平移折进 MVP。
 *            能沿用设备当前布局 / 原点就沿用；精度按可见部分的屏幕误差检查，不够时拆出相机附近的图元
 *     纹理   I16×2，小数位按 |st| 最大值自适应
 *     颜色   全批次相同 → 不带属性，发 COLOR 命令；可重排批次里颜色种类少 → 按颜色分组；否则 RGB565 / RGBA8
 *     法线   仅开光照时带，I8×3 归一化
 *   范围或精度实在不够时退回 F32。
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

enum EnvKind { ENV_REPLACE, ENV_MODULATE, ENV_ADD, ENV_DECAL, ENV_PASS, ENV_ADD_SIGNED };

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
        return ENV_ADD;
    case kGL_ADD_SIGNED:
        return ENV_ADD_SIGNED;
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

static void sync_lighting(Context *c, const Mat4 &mv)
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
        if (!mat_inverse(mv, inv)) {
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

/**
 * 把当前 GL 状态解析成设备侧状态（纯函数，不发命令、不碰纹理驻留）。
 * 两次解析结果相同 = 对设备没有区别，几何可以继续合进同一批次。
 */
void resolve_key(Context *c, DrawKey &k)
{
    memset(&k, 0, sizeof(k));
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
        EnvKind ek = env_kind(us);
        if (ek == ENV_ADD_SIGNED) {
            /* a + b - 0.5：设备没有。第二层多是细节纹理（均值 ≈ 0.5），当透传最接近；第一层按调制 */
            ek = nu == 0 ? ENV_MODULATE : ENV_PASS;
        }
        if (ek == ENV_PASS) {
            continue;
        }
        kinds[nu] = ek;
        k.units[nu++] = u;
    }
    int shade = ESPGPU_SHADE_FLAT;
    if (nu == 1) {
        switch (kinds[0]) {
        case ENV_REPLACE: shade = ESPGPU_SHADE_TEX; break;
        case ENV_ADD: shade = ESPGPU_SHADE_TEX_ADD; break;
        case ENV_DECAL: shade = ESPGPU_SHADE_TEX_DECAL; break;
        default: shade = ESPGPU_SHADE_TEX_MOD_COLOR; break;
        }
    } else if (nu == 2) {
        if (kinds[1] == ENV_REPLACE) {
            /* 第二层 REPLACE 覆盖掉第一层的结果：只剩单元 1 */
            k.units[0] = k.units[1];
            k.units[1] = 0;
            nu = 1;
            shade = ESPGPU_SHADE_TEX;
        } else if (kinds[1] == ENV_ADD) {
            shade = ESPGPU_SHADE_TEX_ADD_TEX;
        } else {
            shade = ESPGPU_SHADE_TEX_MOD_TEX;
        }
        k.white = nu == 2 && kinds[0] == ENV_REPLACE;
    }
    k.nu = nu;
    for (int i = 0; i < nu; i++) {
        const Texture *t = tex_bound(c, k.units[i]);
        k.tex[i] = t;
        k.filt[i] = tex_dev_filter(t);
        k.wrap[i] = tex_dev_wrap(t);
    }

    /* 管线。关掉的功能把参数规范化成固定值：参数变化（如关着雾时改密度）不会白发 PIPELINE */
    espgpu_pipeline_t &p = k.p;
    p.mask = static_cast<uint16_t>(0xFFFF & ~ESPGPU_PS_TEXTURE & ~ESPGPU_PS_SCISSOR);
    p.blend_enable = s.blend ? 1 : 0;
    p.blend_src = s.blend ? map_blend(s.blend_src) : ESPGPU_BLEND_ONE;
    p.blend_dst = s.blend ? map_blend(s.blend_dst) : ESPGPU_BLEND_ZERO;
    p.blend_op = s.blend ? map_blend_op(s.blend_eq) : ESPGPU_BLEND_OP_ADD;
    p.depth_test = s.depth_test ? 1 : 0;
    p.depth_write = (s.depth_test && s.depth_mask) ? 1 : 0;
    p.depth_func = s.depth_test ? map_cmp(s.depth_func) : ESPGPU_CMP_ALWAYS;
    p.alpha_test = s.alpha_test ? 1 : 0;
    p.alpha_func = s.alpha_test ? map_cmp(s.alpha_func) : ESPGPU_CMP_ALWAYS;
    p.alpha_ref = s.alpha_test ? to8(s.alpha_ref) : 0;
    if (s.cull) {
        p.cull_mode = s.cull_face == GL_FRONT ? ESPGPU_CULL_FRONT
                      : s.cull_face == GL_FRONT_AND_BACK ? ESPGPU_CULL_BOTH
                                                         : ESPGPU_CULL_BACK;
        p.front_face = s.front_face == GL_CW ? ESPGPU_FRONT_CW : ESPGPU_FRONT_CCW;
    } else {
        p.cull_mode = ESPGPU_CULL_NONE;
        p.front_face = ESPGPU_FRONT_CCW;
    }
    p.shade_mode = static_cast<uint8_t>(shade);

    k.fog = espgpu_pipeline_fog_t{0.0f, 1.0f, 1.0f};
    if (s.fog) {
        p.fog_enable = 1;
        p.fog_mode = s.fog_mode == GL_LINEAR ? ESPGPU_FOG_LINEAR
                     : s.fog_mode == GL_EXP2 ? ESPGPU_FOG_EXP2
                                             : ESPGPU_FOG_EXP;
        p.fog_color = to565(s.fog_color);
        if (p.fog_mode == ESPGPU_FOG_LINEAR) {
            k.fog.fog_start = s.fog_start;
            k.fog.fog_end = s.fog_end;
        } else {
            k.fog.fog_density = s.fog_density;
        }
    }

    espgpu_pipeline_ext_t &ext = k.ext;
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

    /* 视口 / 裁剪 / 深度范围 */
    win_rect_to_dev(c, s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], k.vp);
    if (s.scissor) {
        win_rect_to_dev(c, s.scissor_box[0], s.scissor_box[1], s.scissor_box[2], s.scissor_box[3], k.sc);
    } else {
        k.sc[0] = k.sc[1] = 0;
        k.sc[2] = static_cast<int16_t>(g_sh.dev_w);
        k.sc[3] = static_cast<int16_t>(g_sh.dev_h);
    }
    k.dr[0] = static_cast<float>(s.depth_range[0]);
    k.dr[1] = static_cast<float>(s.depth_range[1]);

    /* 光照：参与的参数折成一个签名（开着光照时材质 / 灯光变化都要分批） */
    if (s.lighting) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void *d, size_t n) {
            const auto *b = static_cast<const uint8_t *>(d);
            for (size_t i = 0; i < n; i++) {
                h = (h ^ b[i]) * 1099511628211ull;
            }
        };
        mix(s.lights, sizeof(s.lights));
        mix(s.material, sizeof(s.material));
        mix(s.light_model_ambient, sizeof(s.light_model_ambient));
        mix(&s.color_material, sizeof(s.color_material));
        mix(&s.color_material_mode, sizeof(s.color_material_mode));
        k.light_sig = h | 1;
    }

    /* 次序无关：不透明、深度测试 LESS/LEQUAL 且写深度——这类批次之间可以重排合并 */
    k.reorder_ok = p.depth_test && p.depth_write && !p.blend_enable && ext.color_write &&
                   (p.depth_func == ESPGPU_CMP_LESS || p.depth_func == ESPGPU_CMP_LEQUAL);
}

bool key_equal(const DrawKey &a, const DrawKey &b)
{
    return memcmp(&a, &b, sizeof(DrawKey)) == 0;
}

bool sync_device_state(Context *c, int *shade_out, bool *force_white, int dev_units[2], int *n_units)
{
    espgpu_device_t *dev = g_sh.dev;
    if (dev == nullptr) {
        return false;
    }
    device_poll_events();
    sync_viewport_scissor(c);

    DrawKey k;
    resolve_key(c, k);
    int nu = k.nu;
    int shade = k.p.shade_mode;
    dev_units[0] = k.units[0];
    dev_units[1] = k.units[1];

    DevCache &dcb = c->dc;
    if (dcb.bind_epoch != g_sh.bind_epoch) {
        dcb.bound_valid = false;
    }
    /* 纹理驻留 + 绑定：先给本次要用的纹理打上本帧戳，换出时不会选中它们 */
    Texture *tex[2] = {nullptr, nullptr};
    for (int i = 0; i < nu; i++) {
        tex[i] = tex_bound(c, dev_units[i]);
        tex[i]->last_used = g_sh.frame;
        tex[i]->pin = true;
    }
    for (int i = 0; i < nu; i++) {
        const uint32_t e0 = g_sh.bind_epoch;
        if (!tex_make_resident(tex[i])) {
            /* 放不进缓存：退化为少一层纹理，至少不画错 */
            nu = i;
            shade = nu == 0 ? ESPGPU_SHADE_FLAT : ESPGPU_SHADE_TEX_MOD_COLOR;
            break;
        }
        if (g_sh.bind_epoch != e0) {
            dcb.bound_valid = false; /* 设备上的纹理槽变过，已有绑定不可信 */
        }
        if (!dcb.bound_valid) {
            for (int j = i; j < static_cast<int>(ESPGPU_MAX_TEXTURE_UNITS); j++) {
                dcb.bound[j] = 0;
            }
            dcb.bound_valid = true;
        }
        const uint16_t id = tex[i]->dev_id;
        const uint8_t filt = k.filt[i];
        const uint8_t wrap = k.wrap[i];
        if (dcb.bound[i] != id || dcb.bound_filter[i] != filt || dcb.bound_wrap[i] != wrap) {
            espgpu_cmd_bind_texture_uv(dev, static_cast<uint8_t>(i), id, filt, static_cast<uint8_t>(wrap & 0x0F),
                                       static_cast<uint8_t>(wrap >> 4));
            dcb.bound[i] = id;
            dcb.bound_filter[i] = filt;
            dcb.bound_wrap[i] = wrap;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (tex[i]) {
            tex[i]->pin = false;
        }
    }
    if (!dcb.bound_valid) {
        memset(dcb.bound, 0, sizeof(dcb.bound));
        dcb.bound_valid = true;
    }
    dcb.bind_epoch = g_sh.bind_epoch;

    espgpu_pipeline_t p = k.p;
    p.shade_mode = static_cast<uint8_t>(shade);
    DevCache &dc = c->dc;
    if (!dc.valid || memcmp(&p, &dc.pipe, sizeof(p)) != 0 || memcmp(&k.fog, &dc.fog, sizeof(k.fog)) != 0 ||
        memcmp(&k.ext, &dc.ext, sizeof(k.ext)) != 0) {
        espgpu_cmd_pipeline(dev, &p, &k.fog, &k.ext);
        dc.pipe = p;
        dc.fog = k.fog;
        dc.ext = k.ext;
        dc.valid = true;
    }

    sync_lighting(c, c->modelview.top());

    *shade_out = shade;
    *force_white = k.white && nu == 2;
    *n_units = nu;
    return true;
}

/* ── 顶点编码 ─────────────────────────────────────────────────────── */
/*
 * 位置量化坐标系 (origin, frac)：I16 值 = (p - origin) * 2^frac。
 *   - origin 吸附到 2 的幂网格上，frac 位小数的网格点（如 Doom 的 1/128 地图单位）能被精确表示；
 *   - 只要放得下且精度够，就沿用设备上当前的布局与原点（省 VERTEX_LAYOUT + SET_MATRIX，
 *     相邻批次的共享顶点也因此逐位相同，不会出现裂缝）；
 *   - 不能精确表示时按屏幕误差上界检查（离相机近的几何要求高），不够就退回 F32。
 */

namespace {

#ifndef ESPGL_TOL_PX
#define ESPGL_TOL_PX 0.25
#endif
constexpr double kTolPx = ESPGL_TOL_PX; /* 位置量化允许的屏幕误差上界（设备像素，保守估计） */
constexpr double kTolTexel = 1.0 / 16; /* 纹理坐标量化允许的误差（texel） */
constexpr size_t kMaxDrawBody = 24 * 1024 - 64 - 64; /* espgpu 单条命令体上限（kCmdMaxBody）留余量 */
constexpr int kLayoutCmdBytes = 4 + static_cast<int>(sizeof(espgpu_vertex_layout_t));
constexpr int kMatrixCmdBytes = 4 + static_cast<int>(sizeof(espgpu_set_matrix_t)) + 64;

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

struct BatchStats {
    double mn[3] = {1e300, 1e300, 1e300}, mx[3] = {-1e300, -1e300, -1e300};
    bool w_not_one = false;
    bool color_const = true, alpha_one = true;
    const float *c0 = nullptr;
    double tmax[2] = {0, 0};
    size_t n = 0;
};

int frac_for(double extent)
{
    if (!(extent > 0)) {
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

/** 按 Plan 的类型选择排布偏移、生成布局 */
void layout_build(Plan &P)
{
    P.L = espgpu_vertex_layout_t{};
    P.L.index_type = 1;
    uint16_t off = 0;
    P.off_pos = 0;
    if (P.pos_f32) {
        add_attr(P, ESPGPU_ATTR_POSITION, ESPGPU_TYPE_F32, static_cast<uint8_t>(P.pos_comps), 0, 0, 0);
        off = static_cast<uint16_t>(4 * P.pos_comps);
    } else {
        add_attr(P, ESPGPU_ATTR_POSITION, ESPGPU_TYPE_I16, 3, 0, 0, static_cast<uint8_t>(P.pos_frac));
        off = 6;
    }
    for (int k = 0; k < P.ntc; k++) {
        const uint8_t slot = static_cast<uint8_t>(ESPGPU_ATTR_TEXCOORD0 + k);
        if (P.tc_f32[k]) {
            off = align_up(off, 4);
            P.off_tc[k] = off;
            add_attr(P, slot, ESPGPU_TYPE_F32, 2, 0, off, 0);
            off = static_cast<uint16_t>(off + 8);
        } else {
            off = align_up(off, 2);
            P.off_tc[k] = off;
            add_attr(P, slot, ESPGPU_TYPE_I16, 2, 0, off, static_cast<uint8_t>(P.tc_frac[k]));
            off = static_cast<uint16_t>(off + 4);
        }
    }
    if (P.color_mode == 1) {
        off = align_up(off, 2);
        P.off_color = off;
        add_attr(P, ESPGPU_ATTR_COLOR, ESPGPU_TYPE_RGB565, 1, 0, off, 0);
        off = static_cast<uint16_t>(off + 2);
    } else if (P.color_mode == 2) {
        P.off_color = off;
        add_attr(P, ESPGPU_ATTR_COLOR, ESPGPU_TYPE_U8, 4, 1, off, 0);
        off = static_cast<uint16_t>(off + 4);
    }
    if (P.normal) {
        P.off_normal = off;
        add_attr(P, ESPGPU_ATTR_NORMAL, ESPGPU_TYPE_I8, 3, 1, off, 0);
        off = static_cast<uint16_t>(off + 3);
    }
    const uint16_t al = (P.pos_f32 || P.tc_f32[0] || P.tc_f32[1]) ? 4 : 2;
    P.stride = align_up(off, al);
    P.L.stride = P.stride;
}

/**
 * 把设备上现有的布局解析成 Plan（沿用它就不必重发 VERTEX_LAYOUT）。
 * 只认本前端自己会产生的属性形态；tc_units 是批次需要的设备纹理单元数。
 */
bool plan_from_layout(const espgpu_vertex_layout_t &L, int tc_units, Plan &P)
{
    P = Plan();
    P.L = L;
    P.stride = L.stride;
    bool have_pos = false, have_tc[2] = {false, false};
    for (int i = 0; i < L.attrib_count && i < static_cast<int>(ESPGPU_MAX_VERTEX_ATTRIBS); i++) {
        const espgpu_vertex_attrib_t &a = L.attribs[i];
        switch (a.slot) {
        case ESPGPU_ATTR_POSITION:
            P.off_pos = a.offset;
            if (a.type == ESPGPU_TYPE_F32 && (a.components == 3 || a.components == 4)) {
                P.pos_f32 = true;
                P.pos_comps = a.components;
            } else if (a.type == ESPGPU_TYPE_I16 && a.components == 3 && !a.normalized) {
                P.pos_frac = a.frac_bits;
            } else {
                return false;
            }
            have_pos = true;
            break;
        case ESPGPU_ATTR_TEXCOORD0:
        case ESPGPU_ATTR_TEXCOORD1: {
            const int k = a.slot - ESPGPU_ATTR_TEXCOORD0;
            if (a.components != 2 || a.normalized) {
                return false;
            }
            if (a.type == ESPGPU_TYPE_F32) {
                P.tc_f32[k] = true;
            } else if (a.type == ESPGPU_TYPE_I16) {
                P.tc_frac[k] = a.frac_bits;
            } else {
                return false;
            }
            P.off_tc[k] = a.offset;
            have_tc[k] = true;
            break;
        }
        case ESPGPU_ATTR_COLOR:
            P.off_color = a.offset;
            if (a.type == ESPGPU_TYPE_RGB565) {
                P.color_mode = 1;
            } else if (a.type == ESPGPU_TYPE_U8 && a.components == 4 && a.normalized) {
                P.color_mode = 2;
            } else {
                return false;
            }
            break;
        case ESPGPU_ATTR_NORMAL:
            if (a.type != ESPGPU_TYPE_I8 || a.components != 3 || !a.normalized) {
                return false;
            }
            P.off_normal = a.offset;
            P.normal = true;
            break;
        default: return false;
        }
    }
    if (!have_pos) {
        return false;
    }
    /* 设备单元 k 的纹理坐标必须在 TEXCOORDk；多出来的单元无害（编码成 0） */
    for (int k = 0; k < tc_units; k++) {
        if (!have_tc[k]) {
            return false;
        }
    }
    P.ntc = have_tc[1] ? 2 : (have_tc[0] ? 1 : 0);
    return true;
}

/** 预处理：最终顶点色、法线（纹理矩阵在出几何时已经乘过，见 gl_vertex.cpp） */
void preprocess(Context *c, std::vector<Vertex> &v, bool force_white)
{
    const State &s = c->st;
    const bool lit_const = s.lighting && !(s.color_material && (s.color_material_mode == GL_DIFFUSE ||
                                                                 s.color_material_mode == GL_AMBIENT_AND_DIFFUSE));
    if (!force_white && !lit_const && !s.lighting) {
        return;
    }
    for (Vertex &x : v) {
        if (force_white) {
            x.color[0] = x.color[1] = x.color[2] = x.color[3] = 1.0f;
        } else if (lit_const) {
            memcpy(x.color, s.material[0].diffuse, sizeof(x.color));
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

void gather(std::vector<Vertex> *lists[], int nl, const int dev_units[2], int nu, BatchStats &S)
{
    for (int l = 0; l < nl; l++) {
        for (const Vertex &v : *lists[l]) {
            S.n++;
            if (v.pos[3] != 1.0f) {
                S.w_not_one = true;
            }
            for (int k = 0; k < 3; k++) {
                S.mn[k] = std::min(S.mn[k], static_cast<double>(v.pos[k]));
                S.mx[k] = std::max(S.mx[k], static_cast<double>(v.pos[k]));
            }
            if (S.c0 == nullptr) {
                S.c0 = v.color;
            } else if (S.color_const && memcmp(S.c0, v.color, sizeof(v.color)) != 0) {
                S.color_const = false;
            }
            if (v.color[3] < 0.999f) {
                S.alpha_one = false;
            }
            for (int k = 0; k < nu; k++) {
                const float *t = v.tc[dev_units[k]];
                S.tmax[k] = std::max({S.tmax[k], static_cast<double>(std::fabs(t[0])), static_cast<double>(std::fabs(t[1]))});
            }
        }
    }
}

bool pos_fits(const BatchStats &S, const double o[3], int f)
{
    const double sc = std::ldexp(1.0, f);
    for (int k = 0; k < 3; k++) {
        if ((S.mx[k] - o[k]) * sc > 32767.0 || (o[k] - S.mn[k]) * sc > 32767.0) {
            return false;
        }
    }
    return true;
}

bool pos_exact(std::vector<Vertex> *lists[], int nl, const double o[3], int f)
{
    const double sc = std::ldexp(1.0, f);
    for (int l = 0; l < nl; l++) {
        for (const Vertex &v : *lists[l]) {
            for (int k = 0; k < 3; k++) {
                const double q = (static_cast<double>(v.pos[k]) - o[k]) * sc;
                if (q != std::nearbyint(q)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/**
 * 位置误差系数：世界坐标每个分量偏 1 个单位时屏幕上最多偏多少设备像素（保守上界）。
 * 投影后 x_ndc = cx / w，对扰动 δ 求导：|Δx_ndc| ≤ (|r0|₁ + |x_ndc|·|r3|₁)·|δ|∞ / w。
 * 在近平面后面 / 附近的顶点按近平面处的 w 估计（被裁剪出来的交点就在那里）。
 */
/**
 * 位置量化误差模型：世界坐标每个分量偏 1 个单位，图元「可见部分」在屏幕上最多偏多少设备像素。
 * 投影后 x_ndc = cx / w，对扰动 δ 求导：|Δx_ndc| ≤ (|r0|₁ + |x_ndc|·|r3|₁)·|δ|∞ / w。
 * 先把图元按视锥（左右上下近）裁剪，只在裁剪后的多边形顶点上取最坏值：
 * 相机身后、视野外的部分误差再大也看不见（地板贴着近平面的那一截通常在屏幕下方之外）。
 */
struct ErrModel {
    const double *m = nullptr; /* MVP，列主序 */
    double n[4] = {0, 0, 0, 0};
    double vw = 1, vh = 1;

    ErrModel(const Mat4 &mvp, double w, double h) : m(mvp.m), vw(w), vh(h)
    {
        for (int r = 0; r < 4; r++) {
            n[r] = std::fabs(m[0 * 4 + r]) + std::fabs(m[1 * 4 + r]) + std::fabs(m[2 * 4 + r]);
        }
    }

    void clip(const Vertex &v, double out[4]) const
    {
        for (int r = 0; r < 4; r++) {
            out[r] = m[0 * 4 + r] * v.pos[0] + m[1 * 4 + r] * v.pos[1] + m[2 * 4 + r] * v.pos[2] + m[3 * 4 + r] * v.pos[3];
        }
    }

    /** n 个顶点的图元（1 点 / 2 线 / 3、4 面）；整个不可见返回 0 */
    double prim(const Vertex *const *v, int nv) const
    {
        double a[16][4], b[16][4];
        int na = nv;
        for (int i = 0; i < nv; i++) {
            clip(*v[i], a[i]);
        }
        for (int pl = 0; pl < 5 && na > 0; pl++) {
            auto d = [pl](const double *q) {
                switch (pl) {
                case 0: return q[3] + q[0];
                case 1: return q[3] - q[0];
                case 2: return q[3] + q[1];
                case 3: return q[3] - q[1];
                default: return q[3] + q[2];
                }
            };
            int nb = 0;
            for (int i = 0; i < na && nb < 14; i++) {
                const double *p = a[i], *q = a[(i + 1) % na];
                const double dp = d(p), dq = d(q);
                if (dp >= 0) {
                    memcpy(b[nb++], p, sizeof(b[0]));
                }
                if ((dp >= 0) != (dq >= 0)) {
                    const double t = dp / (dp - dq);
                    for (int k = 0; k < 4; k++) {
                        b[nb][k] = p[k] + t * (q[k] - p[k]);
                    }
                    nb++;
                }
            }
            memcpy(a, b, sizeof(a[0]) * static_cast<size_t>(nb));
            na = nb;
        }
        double e = 0;
        for (int i = 0; i < na; i++) {
            const double w = a[i][3];
            if (!(w > 1e-9)) {
                return 1e30; /* 退化投影：当作精度不够 */
            }
            const double xn = std::min(1.0, std::fabs(a[i][0] / w)), yn = std::min(1.0, std::fabs(a[i][1] / w));
            e = std::max(e, std::max(0.5 * vw * (n[0] + xn * n[3]), 0.5 * vh * (n[1] + yn * n[3])) / w);
        }
        return e;
    }

    /** 对批次里每个图元调用 fn(误差系数, 顶点指针数组, 顶点数) */
    template <typename Fn>
    void each(const Pending &g, Fn fn) const
    {
        const Vertex *pv[4];
        for (size_t q = 0; q + 3 < g.quads.size(); q += 4) {
            for (int k = 0; k < 4; k++) pv[k] = &g.quads[q + static_cast<size_t>(k)];
            fn(prim(pv, 4));
        }
        for (size_t k = 0; k + 2 < g.tri_i.size(); k += 3) {
            for (int j = 0; j < 3; j++) pv[j] = &g.tri_v[g.tri_i[k + static_cast<size_t>(j)]];
            fn(prim(pv, 3));
        }
        for (size_t k = 0; k + 1 < g.line_i.size(); k += 2) {
            for (int j = 0; j < 2; j++) pv[j] = &g.line_v[g.line_i[k + static_cast<size_t>(j)]];
            fn(prim(pv, 2));
        }
        for (const Vertex &x : g.points) {
            pv[0] = &x;
            fn(prim(pv, 1));
        }
    }
};

double pos_err_coef(const Pending &g, const Mat4 &mvp, double vw, double vh)
{
    const ErrModel em(mvp, vw, vh);
    double emax = 0;
    em.each(g, [&](double e) { emax = std::max(emax, e); });
    return emax;
}

/** 新坐标系：原点吸附到尽量粗的 2 的幂网格（相邻批次更容易选到同一个原点），返回 false = 放不下 */
bool fresh_frame(const BatchStats &S, double o[3], int &f)
{
    double ext = 0;
    for (int k = 0; k < 3; k++) {
        ext = std::max(ext, (S.mx[k] - S.mn[k]) * 0.5);
    }
    if (!std::isfinite(ext) || ext > 32767.0) {
        return false;
    }
    for (f = frac_for(ext); f >= 0; f--) {
        const double step = std::ldexp(1.0, -f);
        const double range = 32767.0 * step;
        bool ok = true;
        for (int k = 0; k < 3 && ok; k++) {
            const double cmid = (S.mn[k] + S.mx[k]) * 0.5, e = (S.mx[k] - S.mn[k]) * 0.5;
            double g = step;
            while (g * 2 <= 2.0 * (range - e) && g < 65536.0) {
                g *= 2;
            }
            for (;; g *= 0.5) {
                o[k] = std::nearbyint(cmid / g) * g;
                if ((S.mx[k] - o[k]) <= range && (o[k] - S.mn[k]) <= range) {
                    break;
                }
                if (g <= step) {
                    ok = false;
                    break;
                }
            }
        }
        if (ok && pos_fits(S, o, f)) {
            return true;
        }
    }
    return false;
}

bool tc_exact(std::vector<Vertex> *lists[], int nl, int gl_unit, int f)
{
    const double sc = std::ldexp(1.0, f);
    for (int l = 0; l < nl; l++) {
        for (const Vertex &v : *lists[l]) {
            for (int k = 0; k < 2; k++) {
                const double q = static_cast<double>(v.tc[gl_unit][k]) * sc;
                if (q != std::nearbyint(q)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/** frac 位小数的纹理坐标是否够用：放得下，且精确或误差 ≤ kTolTexel */
bool tc_ok(std::vector<Vertex> *lists[], int nl, int gl_unit, double tmax, int dim, int f)
{
    if (tmax * std::ldexp(1.0, f) > 32767.0) {
        return false;
    }
    return 0.5 * std::ldexp(1.0, -f) * dim <= kTolTexel || tc_exact(lists, nl, gl_unit, f);
}

void encode(const Plan &P, const std::vector<Vertex> &v, std::vector<uint8_t> &out)
{
    out.assign(v.size() * P.stride, 0);
    uint8_t *o = out.data();
    const double psc = std::ldexp(1.0, P.pos_frac);
    double tsc[2];
    for (int k = 0; k < 2; k++) {
        tsc[k] = std::ldexp(1.0, P.tc_frac[k]);
    }
    for (const Vertex &x : v) {
        if (P.pos_f32) {
            memcpy(o + P.off_pos, x.pos, 4u * static_cast<unsigned>(P.pos_comps));
        } else {
            int16_t q[3];
            for (int k = 0; k < 3; k++) {
                q[k] = clamp16(std::lround((static_cast<double>(x.pos[k]) - P.origin[k]) * psc));
            }
            memcpy(o + P.off_pos, q, 6);
        }
        for (int k = 0; k < P.ntc; k++) {
            const float *t = x.tc[P.tc_gl_unit[k]];
            if (P.tc_f32[k]) {
                memcpy(o + P.off_tc[k], t, 8);
            } else {
                const int16_t q[2] = {clamp16(std::lround(t[0] * tsc[k])), clamp16(std::lround(t[1] * tsc[k]))};
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
void build_mvp(const Mat4 &proj, const Mat4 &mv, const double origin[3], float out[16])
{
    Mat4 m = mat_mul(proj, mv);
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

int tex_dim_of(Context *c, int gl_unit)
{
    const Texture *t = tex_bound(c, gl_unit);
    const int w = t->dw > 0 ? t->dw : t->w, h = t->dh > 0 ? t->dh : t->h;
    return std::max(1, std::max(w, h));
}

/**
 * 选布局：候选 A = 沿用设备当前布局（+原点），候选 B = 为本批次量身定做。
 * 两者都要满足精度；按「顶点字节 + 需要补发的 LAYOUT / MATRIX / COLOR 命令」挑便宜的。
 */
void plan_build(Context *c, const Mat4 &proj, const Mat4 &mv, const Pending &g, std::vector<Vertex> *lists[], int nl,
                const int dev_units[2], int nu, Plan &out)
{
    DevCache &dc = c->dc;
    BatchStats S;
    gather(lists, nl, dev_units, nu, S);
    const bool lighting = c->st.lighting;
    const Mat4 mvp = mat_mul(proj, mv);
    const double vw = std::max<int>(1, dc.viewport[2]), vh = std::max<int>(1, dc.viewport[3]);
    double ecoef = -1; /* 惰性计算 */
    auto pos_precise = [&](const double o[3], int f) {
        if (pos_exact(lists, nl, o, f)) {
            return true;
        }
        if (ecoef < 0) {
            ecoef = pos_err_coef(g, mvp, vw, vh);
        }
        return 0.5 * std::ldexp(1.0, -f) * ecoef <= kTolPx;
    };
    int dims[2] = {1, 1};
    for (int k = 0; k < nu; k++) {
        dims[k] = tex_dim_of(c, dev_units[k]);
    }
    uint8_t cc[4] = {255, 255, 255, 255};
    if (S.c0) {
        for (int k = 0; k < 4; k++) {
            cc[k] = to8(S.c0[k]);
        }
    }
    auto mvp_cost = [&](const double o[3]) {
        float m[16];
        build_mvp(proj, mv, o, m);
        return (dc.mvp_valid && memcmp(m, dc.mvp, sizeof(m)) == 0) ? 0 : kMatrixCmdBytes;
    };
    auto color_cost = [&](const Plan &P) {
        if (P.color_mode != 0) {
            return 0;
        }
        return (dc.color_valid && memcmp(cc, dc.color, 4) == 0) ? 0 : 8;
    };

    /* 候选 B：量身定做 */
    Plan B;
    B.ntc = nu;
    for (int k = 0; k < nu; k++) {
        B.tc_gl_unit[k] = dev_units[k];
    }
    if (S.w_not_one) {
        B.pos_f32 = true;
        B.pos_comps = 4;
    } else if (!fresh_frame(S, B.origin, B.pos_frac) || !pos_precise(B.origin, B.pos_frac)) {
        B.pos_f32 = true;
        B.pos_comps = 3;
        B.origin[0] = B.origin[1] = B.origin[2] = 0;
    }
    for (int k = 0; k < nu; k++) {
        if (!std::isfinite(S.tmax[k]) || S.tmax[k] >= 32767.0) {
            B.tc_f32[k] = true;
            continue;
        }
        B.tc_frac[k] = frac_for(S.tmax[k]);
        if (!tc_ok(lists, nl, dev_units[k], S.tmax[k], dims[k], B.tc_frac[k])) {
            B.tc_f32[k] = true;
        }
    }
    B.color_mode = S.color_const ? 0 : (S.alpha_one ? 1 : 2);
    B.normal = lighting;
#ifdef ESPGL_TEST_FORCE_F32 /* 测试用：全 F32 作为精度参照 */
    B.pos_f32 = true;
    B.pos_comps = S.w_not_one ? 4 : 3;
    B.origin[0] = B.origin[1] = B.origin[2] = 0;
    B.tc_f32[0] = B.tc_f32[1] = true;
#endif
    layout_build(B);
    const bool b_layout_same = dc.layout_valid && memcmp(&B.L, &dc.layout, sizeof(B.L)) == 0;
    const long cost_b = static_cast<long>(S.n) * B.stride + (b_layout_same ? 0 : kLayoutCmdBytes) +
                        mvp_cost(B.origin) + color_cost(B);

    /* 候选 A：沿用设备当前布局 */
    Plan A;
    bool a_ok = dc.layout_valid && dc.origin_valid && plan_from_layout(dc.layout, nu, A);
#ifdef ESPGL_TEST_FORCE_F32
    a_ok = a_ok && A.pos_f32 && A.tc_f32[0];
#endif
    if (a_ok) {
        for (int k = 0; k < nu; k++) {
            A.tc_gl_unit[k] = dev_units[k];
        }
        if (A.pos_f32) {
            a_ok = !S.w_not_one || A.pos_comps == 4;
            A.origin[0] = A.origin[1] = A.origin[2] = 0;
            if (a_ok && !dc.origin_valid) {
                a_ok = false;
            }
            if (a_ok && (dc.origin[0] != 0 || dc.origin[1] != 0 || dc.origin[2] != 0)) {
                a_ok = false; /* F32 位置不带原点 */
            }
        } else {
            memcpy(A.origin, dc.origin, sizeof(A.origin));
            a_ok = !S.w_not_one && pos_fits(S, A.origin, A.pos_frac) && pos_precise(A.origin, A.pos_frac);
        }
        for (int k = 0; k < nu && a_ok; k++) {
            if (!A.tc_f32[k]) {
                a_ok = std::isfinite(S.tmax[k]) && tc_ok(lists, nl, dev_units[k], S.tmax[k], dims[k], A.tc_frac[k]);
            }
        }
        if (a_ok) {
            if (A.color_mode == 0 && !S.color_const) {
                a_ok = false;
            } else if (A.color_mode == 1 && !S.alpha_one) {
                a_ok = false;
            }
        }
        if (a_ok && lighting && !A.normal) {
            a_ok = false;
        }
    }
    if (a_ok) {
        const long cost_a = static_cast<long>(S.n) * A.stride + mvp_cost(A.origin) + color_cost(A);
        if (cost_a <= cost_b) {
            A.const_color[0] = cc[0];
            A.const_color[1] = cc[1];
            A.const_color[2] = cc[2];
            A.const_color[3] = cc[3];
            out = A;
            return;
        }
    }
    memcpy(B.const_color, cc, 4);
    out = B;
}

} // namespace

/** 发送布局 / MVP / 常量色（与缓存比对，只发变化的） */
static void send_draw_prologue(Context *c, const espgpu_vertex_layout_t &L, const Mat4 &proj, const Mat4 &mv,
                               const double origin[3], bool const_color, const uint8_t cc[4])
{
    espgpu_device_t *dev = g_sh.dev;
    DevCache &dc = c->dc;
    if (!dc.layout_valid || memcmp(&L, &dc.layout, sizeof(L)) != 0) {
        espgpu_cmd_vertex_layout(dev, &L);
        dc.layout = L;
        dc.layout_valid = true;
    }
    float mvp[16];
    build_mvp(proj, mv, origin, mvp);
    if (!dc.mvp_valid || memcmp(mvp, dc.mvp, sizeof(mvp)) != 0) {
        espgpu_cmd_matrix(dev, ESPGPU_MATRIX_MVP, mvp);
        memcpy(dc.mvp, mvp, sizeof(mvp));
        dc.mvp_valid = true;
    }
    memcpy(dc.origin, origin, sizeof(dc.origin));
    dc.origin_valid = true;
    if (const_color && (!dc.color_valid || memcmp(cc, dc.color, 4) != 0)) {
        espgpu_cmd_color(dev, cc[0], cc[1], cc[2], cc[3]);
        memcpy(dc.color, cc, 4);
        dc.color_valid = true;
    }
}

void send_layout_and_matrix(Context *c, const espgpu_vertex_layout_t &L, const double origin[3],
                            bool const_color, const uint8_t cc[4])
{
    send_draw_prologue(c, L, c->projection.top(), c->modelview.top(), origin, const_color, cc);
}

/**
 * 发一条绘制；超过单条命令体上限时按图元边界自己切（保留索引，不让 espgpu 去索引化展开）。
 * idx 为空 = 非索引（QUADS / POINTS）。
 */
static void draw_split(uint8_t prim, const Plan &P, const std::vector<Vertex> &v, const std::vector<uint16_t> *idx,
                       std::vector<uint8_t> &blob)
{
    espgpu_device_t *dev = g_sh.dev;
    const size_t stride = P.stride;
    const size_t ni = idx ? idx->size() : 0;
    if (sizeof(espgpu_draw_t) + v.size() * stride + ni * 2 <= kMaxDrawBody) {
        encode(P, v, blob);
        espgpu_cmd_draw(dev, prim, blob.data(), static_cast<uint32_t>(v.size()), P.stride, idx ? idx->data() : nullptr,
                        static_cast<uint32_t>(ni), 1);
        return;
    }
    const size_t group = prim == ESPGPU_PRIM_QUADS ? 4 : prim == ESPGPU_PRIM_TRIANGLES ? 3 : prim == ESPGPU_PRIM_LINES ? 2 : 1;
    std::vector<Vertex> sub;
    if (idx == nullptr) {
        const size_t per = std::max<size_t>(group, (kMaxDrawBody - sizeof(espgpu_draw_t)) / stride / group * group);
        for (size_t off = 0; off < v.size(); off += per) {
            const size_t n = std::min(per, v.size() - off);
            sub.assign(v.begin() + static_cast<std::ptrdiff_t>(off), v.begin() + static_cast<std::ptrdiff_t>(off + n));
            encode(P, sub, blob);
            espgpu_cmd_draw(dev, prim, blob.data(), static_cast<uint32_t>(n), P.stride, nullptr, 0, 1);
        }
        return;
    }
    /* 索引图元：按图元顺序装箱，顶点按首次出现重映射 */
    std::vector<int32_t> remap(v.size(), -1);
    std::vector<uint16_t> li;
    std::vector<uint32_t> used;
    size_t k = 0;
    while (k + group <= ni) {
        sub.clear();
        li.clear();
        used.clear();
        while (k + group <= ni) {
            size_t add_v = 0;
            for (size_t j = 0; j < group; j++) {
                if (remap[(*idx)[k + j]] < 0) {
                    add_v++;
                }
            }
            const size_t bytes = sizeof(espgpu_draw_t) + (sub.size() + add_v) * stride + (li.size() + group) * 2;
            if (!li.empty() && bytes > kMaxDrawBody) {
                break;
            }
            for (size_t j = 0; j < group; j++) {
                const uint16_t gi = (*idx)[k + j];
                if (remap[gi] < 0) {
                    remap[gi] = static_cast<int32_t>(sub.size());
                    sub.push_back(v[gi]);
                    used.push_back(gi);
                }
                li.push_back(static_cast<uint16_t>(remap[gi]));
            }
            k += group;
        }
        encode(P, sub, blob);
        espgpu_cmd_draw(dev, prim, blob.data(), static_cast<uint32_t>(sub.size()), P.stride, li.data(),
                        static_cast<uint32_t>(li.size()), 1);
        for (uint32_t gi : used) {
            remap[gi] = -1;
        }
    }
}

/** 发出一个批次：临时换上批次开始时的状态快照（设备状态按快照同步），发完换回 */
static int collect_lists(Pending &g, std::vector<Vertex> *lists[4])
{
    int nl = 0;
    if (!g.quads.empty()) lists[nl++] = &g.quads;
    if (!g.tri_v.empty()) lists[nl++] = &g.tri_v;
    if (!g.line_v.empty()) lists[nl++] = &g.line_v;
    if (!g.points.empty()) lists[nl++] = &g.points;
    return nl;
}

/** 规划布局并发出一组同状态的几何 */
static void draw_geometry(Context *c, Pending &g, const Mat4 &proj, const Mat4 &mv, const int dev_units[2], int nu)
{
    std::vector<Vertex> *lists[4];
    const int nl = collect_lists(g, lists);
    if (nl == 0) {
        return;
    }
    Plan P;
    plan_build(c, proj, mv, g, lists, nl, dev_units, nu, P);
    static const bool dbg = env_int("ESPGPU_GL_DEBUG_PLAN", 0) != 0;
    if (dbg) {
        log_line("plan n=%zu f32=%d frac=%d stride=%u color=%d tc=%d,%d", g.total(), P.pos_f32 ? 1 : 0, P.pos_frac,
                 P.stride, P.color_mode, P.tc_frac[0], P.tc_f32[0] ? 1 : 0);
    }
    send_draw_prologue(c, P.L, proj, mv, P.origin, P.color_mode == 0, P.const_color);
    std::vector<uint8_t> &blob = c->scratch_blob;
    if (!g.quads.empty()) {
        draw_split(ESPGPU_PRIM_QUADS, P, g.quads, nullptr, blob);
    }
    if (!g.tri_v.empty() && !g.tri_i.empty()) {
        draw_split(ESPGPU_PRIM_TRIANGLES, P, g.tri_v, &g.tri_i, blob);
    }
    if (!g.line_v.empty() && !g.line_i.empty()) {
        draw_split(ESPGPU_PRIM_LINES, P, g.line_v, &g.line_i, blob);
    }
    if (!g.points.empty()) {
        draw_split(ESPGPU_PRIM_POINTS, P, g.points, nullptr, blob);
    }
}

/**
 * 按图元把 src 分成若干组（组号由 cls(顶点指针数组, 顶点数) 给出，0..ng-1），组内保持原顺序；
 * 索引图元的顶点在各组里重映射（组边界上的共享顶点各留一份）。
 */
template <typename Cls>
static void partition(const Pending &src, int ng, std::vector<Pending> &out, Cls cls)
{
    out.assign(static_cast<size_t>(ng), Pending());
    const Vertex *pv[4];
    for (size_t q = 0; q + 3 < src.quads.size(); q += 4) {
        for (int k = 0; k < 4; k++) pv[k] = &src.quads[q + static_cast<size_t>(k)];
        Pending &dst = out[static_cast<size_t>(cls(pv, 4))];
        dst.quads.insert(dst.quads.end(), src.quads.begin() + static_cast<std::ptrdiff_t>(q),
                         src.quads.begin() + static_cast<std::ptrdiff_t>(q + 4));
    }
    for (const Vertex &x : src.points) {
        pv[0] = &x;
        out[static_cast<size_t>(cls(pv, 1))].points.push_back(x);
    }
    /* 顶点 → (最近放进的组, 组内下标)；同一顶点在组间交替出现时会多复制一份，结果仍正确 */
    std::vector<int32_t> last_gid, last_idx;
    auto split_idx = [&](const std::vector<Vertex> &v, const std::vector<uint16_t> &idx, int group,
                         std::vector<Vertex> Pending::*vm, std::vector<uint16_t> Pending::*im) {
        last_gid.assign(v.size(), -1);
        last_idx.assign(v.size(), 0);
        for (size_t k = 0; k + static_cast<size_t>(group) <= idx.size(); k += static_cast<size_t>(group)) {
            for (int j = 0; j < group; j++) pv[j] = &v[idx[k + static_cast<size_t>(j)]];
            const int gid = cls(pv, group);
            Pending &dst = out[static_cast<size_t>(gid)];
            for (int j = 0; j < group; j++) {
                const uint16_t gi = idx[k + static_cast<size_t>(j)];
                if (last_gid[gi] != gid) {
                    last_gid[gi] = gid;
                    last_idx[gi] = static_cast<int32_t>((dst.*vm).size());
                    (dst.*vm).push_back(v[gi]);
                }
                (dst.*im).push_back(static_cast<uint16_t>(last_idx[gi]));
            }
        }
    };
    split_idx(src.tri_v, src.tri_i, 3, &Pending::tri_v, &Pending::tri_i);
    split_idx(src.line_v, src.line_i, 2, &Pending::line_v, &Pending::line_i);
    for (Pending &g : out) {
        g.classes = 0;
        if (!g.quads.empty()) g.classes |= 1 << PCLS_QUADS;
        if (!g.tri_i.empty()) g.classes |= 1 << PCLS_TRIS;
        if (!g.line_i.empty()) g.classes |= 1 << PCLS_LINES;
        if (!g.points.empty()) g.classes |= 1 << PCLS_POINTS;
    }
}

/**
 * 批次跨度大、离相机又近时 I16 精度不够：与其整批退回 F32（每顶点 +6 字节），
 * 不如把「整批坐标系下精度够」的图元和其余（通常是相机附近的一小片）分开，各自量化。
 * 只用于可重排的批次（拆开后图元顺序会变）。成功返回 true。
 */
static bool split_by_precision(Context *c, const Pending &p, std::vector<Pending> &parts)
{
    std::vector<Vertex> *lists[4];
    const int nl = collect_lists(const_cast<Pending &>(p), lists);
    const int units[2] = {0, 1};
    BatchStats S;
    gather(lists, nl, units, 0, S);
    double o[3];
    int f = 0;
    if (S.w_not_one || !fresh_frame(S, o, f)) {
        return false;
    }
    const ErrModel em(mat_mul(p.proj, p.mv), std::max<int>(1, c->dc.viewport[2]), std::max<int>(1, c->dc.viewport[3]));
    const double h = 0.5 * std::ldexp(1.0, -f);
    const double sc = std::ldexp(1.0, f);
    auto exact = [&](const Vertex &v) {
        for (int k = 0; k < 3; k++) {
            const double q = (static_cast<double>(v.pos[k]) - o[k]) * sc;
            if (q != std::nearbyint(q)) {
                return false;
            }
        }
        return true;
    };
    partition(p, 2, parts, [&](const Vertex *const *pv, int nv) {
        bool ex = true;
        for (int k = 0; k < nv && ex; k++) {
            ex = exact(*pv[k]);
        }
        return (ex || h * em.prim(pv, nv) <= kTolPx) ? 0 : 1;
    });
    return !parts[0].empty() && !parts[1].empty();
}

/**
 * 可重排批次里顶点色只有少数几种（Doom：几档光照）时，按颜色分组、每组发一条 COLOR + DRAW，
 * 比每个顶点带 2~4 字节颜色省。颜色不统一的图元归到最后一组（照旧逐顶点带色）。
 */
static bool split_by_color(const Pending &p, std::vector<Pending> &parts)
{
    constexpr int kMaxGroups = 16;
    float cols[kMaxGroups][4];
    int ng = 0;
    bool overflow = false;
    size_t nverts = p.total();
    auto group_of = [&](const Vertex *const *pv, int nv, bool add) {
        for (int k = 1; k < nv; k++) {
            if (memcmp(pv[k]->color, pv[0]->color, sizeof(pv[0]->color)) != 0) {
                return kMaxGroups; /* 混色图元 */
            }
        }
        for (int g = 0; g < ng; g++) {
            if (memcmp(cols[g], pv[0]->color, sizeof(cols[g])) == 0) {
                return g;
            }
        }
        if (add && ng < kMaxGroups) {
            memcpy(cols[ng], pv[0]->color, sizeof(cols[ng]));
            return ng++;
        }
        overflow = true;
        return kMaxGroups;
    };
    /* 先数组数，划不来就不拆 */
    std::vector<Pending> probe;
    partition(p, kMaxGroups + 1, probe, [&](const Vertex *const *pv, int nv) { return group_of(pv, nv, true); });
    if (overflow || ng < 2) {
        return false;
    }
    bool alpha_one = true;
    for (int g = 0; g < ng; g++) {
        alpha_one = alpha_one && cols[g][3] >= 0.999f;
    }
    const size_t cbytes = alpha_one ? 2 : 4;
    const size_t mixed = probe[kMaxGroups].total();
    const size_t saved = (nverts - mixed) * cbytes;
    const size_t cost = static_cast<size_t>(ng) * (4 + sizeof(espgpu_draw_t) + 8) + kLayoutCmdBytes;
    if (saved < 2 * cost) {
        return false;
    }
    parts.clear();
    for (int g = 0; g < ng; g++) {
        parts.push_back(std::move(probe[static_cast<size_t>(g)]));
    }
    if (mixed) {
        parts.push_back(std::move(probe[kMaxGroups]));
    }
    return true;
}

/** 发出一个批次：临时换上批次开始时的状态快照（设备状态按快照同步），发完换回 */
static void flush_one(Context *c, Pending &p)
{
    if (p.empty()) {
        return;
    }
    if (g_sh.dev == nullptr) {
        p.clear();
        return;
    }
    std::swap(c->st, p.st);
    int shade = 0, nu = 0;
    bool white = false;
    int dev_units[2] = {0, 0};
    const Mat4 mv_live = c->modelview.top();
    c->modelview.top() = p.mv; /* 光照的法线矩阵按批次的 modelview */
    const bool ok = sync_device_state(c, &shade, &white, dev_units, &nu);
    c->modelview.top() = mv_live;
    if (ok) {
        std::vector<Vertex> *lists[4];
        const int nl = collect_lists(p, lists);
        for (int l = 0; l < nl; l++) {
            preprocess(c, *lists[l], white);
        }
        /* 可重排的批次可以再按精度 / 颜色拆开发（图元顺序会变）。几何搬到工作区，快照留在 p 里 */
        std::vector<Pending> parts(1), sub;
        Pending &w = parts[0];
        w.quads.swap(p.quads);
        w.tri_v.swap(p.tri_v);
        w.tri_i.swap(p.tri_i);
        w.line_v.swap(p.line_v);
        w.line_i.swap(p.line_i);
        w.points.swap(p.points);
        w.classes = p.classes;
        w.proj = p.proj;
        w.mv = p.mv;
        if (p.key.reorder_ok && w.total() >= 32) {
            Plan P;
            std::vector<Vertex> *wl[4];
            const int wn = collect_lists(w, wl);
            plan_build(c, w.proj, w.mv, w, wl, wn, dev_units, nu, P);
            if (P.pos_f32 && P.pos_comps == 3 && split_by_precision(c, w, sub)) {
                parts = std::move(sub);
                sub.clear();
            }
            std::vector<Pending> out;
            for (Pending &g : parts) {
                if (split_by_color(g, sub)) {
                    for (Pending &x : sub) {
                        out.push_back(std::move(x));
                    }
                } else {
                    out.push_back(std::move(g));
                }
            }
            parts = std::move(out);
        }
        for (Pending &g : parts) {
            draw_geometry(c, g, p.proj, p.mv, dev_units, nu);
        }
        /* 把（可能最大的）顶点缓冲还给 p，下一批次复用容量 */
        if (!parts.empty()) {
            parts[0].clear();
            p.quads.swap(parts[0].quads);
            p.tri_v.swap(parts[0].tri_v);
            p.tri_i.swap(parts[0].tri_i);
        }
    }
    std::swap(c->st, p.st);
    p.clear();
}

void flush_current(Context *c)
{
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    flush_one(c, c->pending);
}

void flush_pending(Context *c)
{
    if (c->parked.empty() && c->pending.empty()) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    /* 按批次开始的先后发：暂存的批次都可重排，但保持先后能让结果与逐条执行最接近 */
    std::vector<Pending *> order;
    order.reserve(c->parked.size() + 1);
    for (Pending &b : c->parked) {
        order.push_back(&b);
    }
    if (!c->pending.empty()) {
        order.push_back(&c->pending);
    }
    std::sort(order.begin(), order.end(), [](const Pending *a, const Pending *b) { return a->seq < b->seq; });
    for (Pending *b : order) {
        flush_one(c, *b);
    }
    c->parked.clear();
}

/* 显示列表缓存用：按列表整体规划布局并编码（gl_list.cpp） */
bool list_plan_encode(Context *c, std::vector<Vertex> &all, bool want_color, bool want_tc[2], bool want_normal,
                      espgpu_vertex_layout_t &L, double origin[3], std::vector<uint8_t> &blob)
{
    (void)c;
    std::vector<Vertex> *lists[1] = {&all};
    const int units[2] = {0, 1};
    BatchStats S;
    gather(lists, 1, units, 2, S);
    Plan P;
    /* 列表的 MVP 在回放时才知道：位置只要求放得下（原点吸附网格，网格点上的坐标精确）。
     * 范围按 2 的幂取整再定小数位：大小相近的列表布局相同，回放时不用每次重发 VERTEX_LAYOUT */
    for (int k = 0; k < 3; k++) {
        const double e = (S.mx[k] - S.mn[k]) * 0.5;
        const double e2 = e > 0 ? std::ldexp(1.0, static_cast<int>(std::ceil(std::log2(e)))) : 0.0;
        const double cmid = (S.mn[k] + S.mx[k]) * 0.5;
        if (std::isfinite(e2)) {
            S.mn[k] = std::min(S.mn[k], cmid - e2);
            S.mx[k] = std::max(S.mx[k], cmid + e2);
        }
    }
    for (int u = 0; u < 2; u++) {
        if (S.tmax[u] > 0 && std::isfinite(S.tmax[u])) {
            S.tmax[u] = std::ldexp(1.0, static_cast<int>(std::ceil(std::log2(S.tmax[u]))));
        }
    }
    if (S.w_not_one || !fresh_frame(S, P.origin, P.pos_frac)) {
        P.pos_f32 = true;
        P.pos_comps = 4;
        P.origin[0] = P.origin[1] = P.origin[2] = 0;
    }
    /* 列表缓存里纹理坐标按 GL 单元存：设备单元 k ← GL 单元 k（回放时要求两者一一对应） */
    int ntc = 0;
    for (int u = 0; u < kMaxUnits; u++) {
        if (!want_tc[u]) {
            break; /* 只支持前缀连续的单元：TEXCOORDk 必须是 GL 单元 k */
        }
        P.tc_gl_unit[u] = u;
        ntc = u + 1;
        if (!std::isfinite(S.tmax[u]) || S.tmax[u] >= 32767.0) {
            P.tc_f32[u] = true;
        } else {
            P.tc_frac[u] = frac_for(S.tmax[u]);
            /* 纹理尺寸回放时才确定：按 512 估计误差 */
            if (!tc_ok(lists, 1, u, S.tmax[u], 512, P.tc_frac[u])) {
                P.tc_f32[u] = true;
            }
        }
    }
    P.ntc = ntc;
    P.color_mode = want_color ? 2 : 0;
    P.normal = want_normal;
    layout_build(P);
    encode(P, all, blob);
    L = P.L;
    origin[0] = P.origin[0];
    origin[1] = P.origin[1];
    origin[2] = P.origin[2];
    return !blob.empty();
}

} // namespace espgl
