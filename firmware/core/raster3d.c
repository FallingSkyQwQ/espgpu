/*
 * raster3d.c — tile 光栅器
 *
 * PRESENT（或 arena 满、纹理被改、FBO 切换）时逐 tile 执行：
 *   1. 取 tile 的颜色 / 深度到内部 SRAM（屏幕目标本来就在 SRAM 时颜色直接写）
 *   2. 依序执行分箱记录：CLEAR 矩形、三角形
 *   3. 写回
 *
 * 三角形按行求跨度（边函数解析求交，不逐像素做内测试），跨度内每 8 像素做一次
 * 精确透视除法、中间线性插值（经典的 Quake 式分段透视校正）。
 *
 * 内层循环按"状态组合"分三条：
 *   fill   —— 常量色、不混合、无深度：UI / 背景
 *   fast   —— 单纹理（RGB565）× 常量色、最近邻、不混合、无 alpha 测试 / 雾，可带深度：
 *             2D 背景与 Doom 墙面地面的主路径
 *   generic—— 其它一切（多纹理、混合、雾、alpha 测试、片元 VM）
 */

#include <math.h>

#include "core_internal.h"

#define SEG 8

typedef struct {
    gpu_core_t *c;
    gp_worker_t *w;
    const gp_target_t *tg;
    int tx0, ty0, tx1, ty1;   /* tile 像素范围（含） */
    uint16_t *cbuf;           /* tile 左上角对应的颜色指针 */
    int cstride;
    uint16_t *dbuf;           /* tile 深度（stride = GP_TILE） */
    bool depth_dirty;
    uint8_t *dbg;             /* 屏幕目标的调试缓冲，可空 */
    uint8_t dbg_mode;
} tile_ctx_t;

/* ── 混合 ─────────────────────────────────────────────────────────── */

static inline uint32_t blend_factor(uint8_t f, uint32_t src_c, uint32_t dst_c, uint32_t sa)
{
    switch (f) {
    case ESPGPU_BLEND_ZERO: return 0;
    case ESPGPU_BLEND_ONE: return 255;
    case ESPGPU_BLEND_SRC_COLOR: return src_c;
    case ESPGPU_BLEND_ONE_MINUS_SRC_COLOR: return 255 - src_c;
    case ESPGPU_BLEND_DST_COLOR: return dst_c;
    case ESPGPU_BLEND_ONE_MINUS_DST_COLOR: return 255 - dst_c;
    case ESPGPU_BLEND_SRC_ALPHA: return sa;
    case ESPGPU_BLEND_ONE_MINUS_SRC_ALPHA: return 255 - sa;
    case ESPGPU_BLEND_DST_ALPHA: return 255; /* 帧缓冲无 alpha，视为 1 */
    case ESPGPU_BLEND_ONE_MINUS_DST_ALPHA: return 0;
    default: return 255;
    }
}

static inline uint32_t blend_ch(const gp_rstate_t *s, uint32_t sc, uint32_t dc, uint32_t sa)
{
    const uint32_t a = gp_mul8(sc, blend_factor(s->bsrc, sc, dc, sa));
    const uint32_t b = gp_mul8(dc, blend_factor(s->bdst, sc, dc, sa));
    int32_t r;
    switch (s->bop) {
    case ESPGPU_BLEND_OP_SUB: r = (int32_t)a - (int32_t)b; break;
    case ESPGPU_BLEND_OP_REV_SUB: r = (int32_t)b - (int32_t)a; break;
    case ESPGPU_BLEND_OP_MIN: r = (int32_t)(sc < dc ? sc : dc); break;
    case ESPGPU_BLEND_OP_MAX: r = (int32_t)(sc > dc ? sc : dc); break;
    default: r = (int32_t)(a + b); break;
    }
    return (uint32_t)gp_clampi(r, 0, 255);
}

/* ── 片元着色（定点 0..255） ─────────────────────────────────────── */

static inline uint32_t mod4(uint32_t a, uint32_t b)
{
    /* 逐通道 a*b/255，ARGB */
    return (gp_mul8(a >> 24, b >> 24) << 24) | (gp_mul8((a >> 16) & 255, (b >> 16) & 255) << 16) |
           (gp_mul8((a >> 8) & 255, (b >> 8) & 255) << 8) | gp_mul8(a & 255, b & 255);
}

static inline uint32_t add_rgb(uint32_t a, uint32_t b, uint32_t alpha)
{
    uint32_t r = ((a >> 16) & 255) + ((b >> 16) & 255);
    uint32_t g = ((a >> 8) & 255) + ((b >> 8) & 255);
    uint32_t bl = (a & 255) + (b & 255);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (bl > 255) bl = 255;
    return (alpha << 24) | (r << 16) | (g << 8) | bl;
}

static inline uint32_t shade_fixed(uint8_t mode, uint32_t C, uint32_t T0, uint32_t T1)
{
    switch (mode) {
    case ESPGPU_SHADE_FLAT: return C;
    case ESPGPU_SHADE_TEX: return T0;
    case ESPGPU_SHADE_TEX_MOD_COLOR: return mod4(T0, C);
    case ESPGPU_SHADE_TEX_MOD_TEX: return mod4(mod4(T0, T1), C);
    case ESPGPU_SHADE_TEX_ADD: return add_rgb(T0, C, gp_mul8(T0 >> 24, C >> 24));
    case ESPGPU_SHADE_TEX_ADD_TEX: {
        const uint32_t m = mod4(T0, C);
        return add_rgb(m, T1, m >> 24);
    }
    case ESPGPU_SHADE_TEX_MOD_ADD_COLOR: {
        const uint32_t m = mod4(T0, T1);
        return add_rgb(m, C, T0 >> 24);
    }
    case ESPGPU_SHADE_TEX_DECAL: {
        const int32_t ta = (int32_t)(T0 >> 24);
        uint32_t o = C & 0xFF000000u;
        for (int sh = 0; sh <= 16; sh += 8) {
            const int32_t cc = (int32_t)((C >> sh) & 255);
            const int32_t tt = (int32_t)((T0 >> sh) & 255);
            const int32_t v = cc + ((tt - cc) * ta + 127) / 255;
            o |= (uint32_t)gp_clampi(v, 0, 255) << sh;
        }
        return o;
    }
    default: return C;
    }
}

static inline uint32_t float_rgba(const float *v)
{
    const int r = (int)(gp_clampf(v[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    const int g = (int)(gp_clampf(v[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    const int b = (int)(gp_clampf(v[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    const int a = (int)(gp_clampf(v[3], 0.0f, 1.0f) * 255.0f + 0.5f);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ── 跨度求解 ─────────────────────────────────────────────────────── */

/** 在 [lo, hi] 内求三条边都 >= 0 的连续跨度，空则返回 false */
static inline bool row_span(const gp_tri_t *t, int y, int lo, int hi, int *out_l, int *out_r)
{
    int L = lo, R = hi;
    for (int k = 0; k < 3; k++) {
        const int64_t a = t->ea[k];
        const int64_t Ey = (int64_t)t->eb[k] * y + t->ec[k];
        if (a == 0) {
            if (Ey < 0) {
                return false;
            }
            continue;
        }
        float est = -(float)Ey * t->inv_ea[k];
        if (est < (float)(lo - 1)) est = (float)(lo - 1);
        if (est > (float)(hi + 1)) est = (float)(hi + 1);
        if (a > 0) {
            int x = (int)ceilf(est);
            while (x <= hi && a * x + Ey < 0) x++;
            while (x > lo && a * (x - 1) + Ey >= 0) x--;
            if (x > L) L = x;
        } else {
            int x = (int)floorf(est);
            while (x >= lo && a * x + Ey < 0) x--;
            while (x < hi && a * (x + 1) + Ey >= 0) x++;
            if (x < R) R = x;
        }
        if (L > R) {
            return false;
        }
    }
    *out_l = L;
    *out_r = R;
    return L <= R;
}

static inline float plane_at(const gp_plane_t *p, float x, float y)
{
    return p->c + p->dx * x + p->dy * y;
}

/* ── 通用路径 ─────────────────────────────────────────────────────── */

GP_HOT static void raster_tri_generic(tile_ctx_t *tc, const gp_tri_t *t, const gp_rstate_t *s)
{
    gpu_core_t *c = tc->c;
    const int n = t->n_att;
    const bool affine = (t->flags & GP_TF_AFFINE) != 0;
    const bool const_color = (t->flags & GP_TF_CONST_COLOR) != 0;
    const bool vm = (t->flags & GP_TF_VM) != 0 && s->vm && s->prog;

    const int o_col = t->o_col, o_t0 = t->o_t0, o_t1 = t->o_t1, o_fog = t->o_fog;
    (void)const_color;

    const uint32_t Cconst = ((uint32_t)t->const_rgba[3] << 24) | ((uint32_t)t->const_rgba[0] << 16) |
                            ((uint32_t)t->const_rgba[1] << 8) | t->const_rgba[2];
    const bool dtest = s->depth_test != 0;
    const bool dwrite = s->depth_write != 0;
    const uint8_t dfunc = s->depth_func;
    const bool cwrite = s->color_write != 0;
    const bool blend = s->blend != 0;
    const bool atest = s->alpha_test != 0;
    const uint8_t afunc = s->alpha_func;
    const uint32_t aref = s->alpha_ref;
    const bool fog = s->fog && o_fog >= 0;
    const gp_sampler_t *s0 = s->smp[0].tex ? &s->smp[0] : NULL;
    const gp_sampler_t *s1 = s->smp[1].tex ? &s->smp[1] : NULL;
    const uint8_t mode = s->shade;
    uint8_t *dbg = tc->dbg;
    const uint8_t dbg_mode = tc->dbg_mode;
    const int sw = tc->tg->w;

    const int y0 = t->by0 > tc->ty0 ? t->by0 : tc->ty0;
    const int y1 = t->by1 < tc->ty1 ? t->by1 : tc->ty1;
    const int lo = t->bx0 > tc->tx0 ? t->bx0 : tc->tx0;
    const int hi = t->bx1 < tc->tx1 ? t->bx1 : tc->tx1;

    float va[GP_MAX_ATT];
    float vs[GP_MAX_ATT];
    float vstep[GP_MAX_ATT];
    float vmv[ESPGPU_VM_VARYINGS * 4];
    uint32_t frags = 0;

    for (int y = y0; y <= y1; y++) {
        int L, R;
        if (!row_span(t, y, lo, hi, &L, &R)) {
            continue;
        }
        uint16_t *crow = tc->cbuf + (size_t)(y - tc->ty0) * tc->cstride - tc->tx0;
        uint16_t *drow = tc->dbuf ? tc->dbuf + (size_t)(y - tc->ty0) * GP_TILE - tc->tx0 : NULL;
        const float fy = (float)y;
        float z = plane_at(&t->z, (float)L, fy);
        const float dz = t->z.dx;

        for (int xs = L; xs <= R; xs += SEG) {
            const int len = (R - xs + 1) < SEG ? (R - xs + 1) : SEG;
            const float fx = (float)xs;
            if (affine) {
                for (int k = 0; k < n; k++) {
                    vs[k] = plane_at(&t->att[k], fx, fy);
                    vstep[k] = t->att[k].dx;
                }
            } else {
                const float iw_s = plane_at(&t->iw, fx, fy);
                const float iw_e = plane_at(&t->iw, fx + (float)len, fy);
                const float ws = iw_s != 0.0f ? 1.0f / iw_s : 0.0f;
                const float we = iw_e != 0.0f ? 1.0f / iw_e : 0.0f;
                const float inv_len = 1.0f / (float)len;
                for (int k = 0; k < n; k++) {
                    const float as = plane_at(&t->att[k], fx, fy) * ws;
                    const float ae = plane_at(&t->att[k], fx + (float)len, fy) * we;
                    vs[k] = as;
                    vstep[k] = (ae - as) * inv_len;
                }
            }
            for (int k = 0; k < n; k++) {
                va[k] = vs[k];
            }

            for (int i = 0; i < len; i++, z += dz) {
                const int x = xs + i;
                uint32_t zi = 0;
                if (dtest || dwrite) {
                    zi = (uint32_t)gp_clampi((int)z, 0, 65535);
                }
                if (dtest && !gp_cmp(dfunc, zi, drow[x])) {
                    goto next;
                }

                uint32_t out;
                if (vm) {
                    memset(vmv, 0, sizeof(vmv));
                    const int nv = (o_fog >= 0 ? n - 1 : n);
                    memcpy(vmv, va, sizeof(float) * (size_t)nv);
                    vmv[24] = (float)x + 0.5f;
                    vmv[25] = fy + 0.5f;
                    vmv[26] = z / 65535.0f;
                    vmv[27] = affine ? t->iw.c : plane_at(&t->iw, (float)x, fy);
                    float o[4];
                    if (!gp_vm_run(s->prog, s->consts, s->smp, vmv, tc->w->vm_regs, o)) {
                        goto next;
                    }
                    out = float_rgba(o);
                } else {
                    const uint32_t C = o_col >= 0 ? float_rgba(&va[o_col]) : Cconst;
                    const uint32_t T0 =
                        (s0 && o_t0 >= 0) ? gp_sample(s0, va[o_t0], va[o_t0 + 1]) : 0xFFFFFFFFu;
                    const uint32_t T1 =
                        (s1 && o_t1 >= 0) ? gp_sample(s1, va[o_t1], va[o_t1 + 1]) : 0xFFFFFFFFu;
                    out = shade_fixed(mode, C, T0, T1);
                }

                if (fog) {
                    const uint32_t f = (uint32_t)(gp_clampf(va[o_fog], 0.0f, 1.0f) * 255.0f + 0.5f);
                    const uint32_t r = gp_mul8((out >> 16) & 255, f) + gp_mul8(s->fog_r, 255 - f);
                    const uint32_t g = gp_mul8((out >> 8) & 255, f) + gp_mul8(s->fog_g, 255 - f);
                    const uint32_t b = gp_mul8(out & 255, f) + gp_mul8(s->fog_b, 255 - f);
                    out = (out & 0xFF000000u) | ((r > 255 ? 255 : r) << 16) |
                          ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
                }

                if (atest && !gp_cmp(afunc, out >> 24, aref)) {
                    goto next;
                }

                if (cwrite) {
                    if (blend) {
                        const uint32_t d = gp_expand565(crow[x]);
                        const uint32_t sa = out >> 24;
                        const uint32_t r = blend_ch(s, (out >> 16) & 255, (d >> 16) & 255, sa);
                        const uint32_t g = blend_ch(s, (out >> 8) & 255, (d >> 8) & 255, sa);
                        const uint32_t b = blend_ch(s, out & 255, d & 255, sa);
                        crow[x] = gp_pack565(r, g, b);
                    } else {
                        crow[x] = gp_pack565((out >> 16) & 255, (out >> 8) & 255, out & 255);
                    }
                }
                if (dwrite) {
                    drow[x] = (uint16_t)zi;
                }
                frags++;
                if (dbg && (dbg_mode == ESPGPU_DEBUG_VIEW_OVERDRAW ||
                            dbg_mode == ESPGPU_DEBUG_VIEW_TILES)) {
                    uint8_t *p = &dbg[(size_t)y * sw + x];
                    if (*p < 255) (*p)++;
                }
            next:
                for (int k = 0; k < n; k++) {
                    va[k] += vstep[k];
                }
            }
        }
    }
    tc->w->fragments += frags;
    (void)c;
}

/* ── 快速路径：RGB565 纹理 × 常量色、最近邻、不混合 ────────────── */

GP_HOT static void raster_tri_fast(tile_ctx_t *tc, const gp_tri_t *t, const gp_rstate_t *s)
{
    const gp_sampler_t *sm = &s->smp[0];
    const gp_tex_t *tex = sm->tex;
    const uint16_t *tpx = tex->px;
    const int tw = tex->w, th = tex->h;
    const float ftw = (float)tw, fth = (float)th;
    const bool rep_u = sm->wrap_u != ESPGPU_WRAP_CLAMP;
    const bool rep_v = sm->wrap_v != ESPGPU_WRAP_CLAMP;
    const bool mirror_u = sm->wrap_u == ESPGPU_WRAP_MIRROR;
    const bool mirror_v = sm->wrap_v == ESPGPU_WRAP_MIRROR;
    const bool affine = (t->flags & GP_TF_AFFINE) != 0;
    const bool dtest = s->depth_test != 0;
    const bool dwrite = s->depth_write != 0;
    const uint8_t dfunc = s->depth_func;

    const bool modulate = s->shade == ESPGPU_SHADE_TEX_MOD_COLOR &&
                          !(t->const_rgba[0] == 255 && t->const_rgba[1] == 255 &&
                            t->const_rgba[2] == 255);
    const uint32_t mr = t->const_rgba[0] + 1u, mg = t->const_rgba[1] + 1u,
                   mb = t->const_rgba[2] + 1u;

    const int y0 = t->by0 > tc->ty0 ? t->by0 : tc->ty0;
    const int y1 = t->by1 < tc->ty1 ? t->by1 : tc->ty1;
    const int lo = t->bx0 > tc->tx0 ? t->bx0 : tc->tx0;
    const int hi = t->bx1 < tc->tx1 ? t->bx1 : tc->tx1;
    const gp_plane_t *pu = &t->att[0];
    const gp_plane_t *pv = &t->att[1];
    uint32_t frags = 0;

    for (int y = y0; y <= y1; y++) {
        int L, R;
        if (!row_span(t, y, lo, hi, &L, &R)) {
            continue;
        }
        uint16_t *crow = tc->cbuf + (size_t)(y - tc->ty0) * tc->cstride - tc->tx0;
        uint16_t *drow = tc->dbuf ? tc->dbuf + (size_t)(y - tc->ty0) * GP_TILE - tc->tx0 : NULL;
        const float fy = (float)y;
        float z = plane_at(&t->z, (float)L, fy);
        const float dz = t->z.dx;

        for (int xs = L; xs <= R; xs += SEG) {
            const int len = (R - xs + 1) < SEG ? (R - xs + 1) : SEG;
            const float fx = (float)xs;
            float u, v, du, dv;
            if (affine) {
                u = plane_at(pu, fx, fy);
                v = plane_at(pv, fx, fy);
                du = pu->dx;
                dv = pv->dx;
            } else {
                const float iw_s = plane_at(&t->iw, fx, fy);
                const float iw_e = plane_at(&t->iw, fx + (float)len, fy);
                const float ws = iw_s != 0.0f ? 1.0f / iw_s : 0.0f;
                const float we = iw_e != 0.0f ? 1.0f / iw_e : 0.0f;
                const float inv_len = 1.0f / (float)len;
                u = plane_at(pu, fx, fy) * ws;
                v = plane_at(pv, fx, fy) * ws;
                du = (plane_at(pu, fx + (float)len, fy) * we - u) * inv_len;
                dv = (plane_at(pv, fx + (float)len, fy) * we - v) * inv_len;
            }
            /* REPEAT：把起点折回 [0, n)，循环内只需偶尔加减一次 */
            if (rep_u && !mirror_u && (u < 0.0f || u >= ftw)) {
                u -= floorf(u / ftw) * ftw;
            }
            if (rep_v && !mirror_v && (v < 0.0f || v >= fth)) {
                v -= floorf(v / fth) * fth;
            }

            for (int i = 0; i < len; i++, z += dz, u += du, v += dv) {
                const int x = xs + i;
                uint32_t zi = 0;
                if (dtest || dwrite) {
                    zi = (uint32_t)gp_clampi((int)z, 0, 65535);
                    if (dtest && !gp_cmp(dfunc, zi, drow[x])) {
                        continue;
                    }
                }
                int iu = (int)u;
                if (u < 0.0f) iu = (int)floorf(u);
                int iv = (int)v;
                if (v < 0.0f) iv = (int)floorf(v);
                if (iu < 0 || iu >= tw) {
                    if (mirror_u) {
                        int m = iu % (2 * tw);
                        if (m < 0) m += 2 * tw;
                        iu = m < tw ? m : 2 * tw - 1 - m;
                    } else if (rep_u) {
                        iu %= tw;
                        if (iu < 0) iu += tw;
                    } else {
                        iu = iu < 0 ? 0 : tw - 1;
                    }
                }
                if (iv < 0 || iv >= th) {
                    if (mirror_v) {
                        int m = iv % (2 * th);
                        if (m < 0) m += 2 * th;
                        iv = m < th ? m : 2 * th - 1 - m;
                    } else if (rep_v) {
                        iv %= th;
                        if (iv < 0) iv += th;
                    } else {
                        iv = iv < 0 ? 0 : th - 1;
                    }
                }
                uint16_t px = tpx[(size_t)iv * tw + iu];
                if (modulate) {
                    const uint32_t r = (((uint32_t)(px >> 11) & 0x1F) * mr) >> 8;
                    const uint32_t g = (((uint32_t)(px >> 5) & 0x3F) * mg) >> 8;
                    const uint32_t b = (((uint32_t)px & 0x1F) * mb) >> 8;
                    px = (uint16_t)((r << 11) | (g << 5) | b);
                }
                crow[x] = px;
                if (dwrite) {
                    drow[x] = (uint16_t)zi;
                }
                frags++;
            }
        }
    }
    tc->w->fragments += frags;
}

/* ── 常量色填充 ───────────────────────────────────────────────────── */

GP_HOT static void raster_tri_fill(tile_ctx_t *tc, const gp_tri_t *t)
{
    const uint16_t col = gp_pack565(t->const_rgba[0], t->const_rgba[1], t->const_rgba[2]);
    const int y0 = t->by0 > tc->ty0 ? t->by0 : tc->ty0;
    const int y1 = t->by1 < tc->ty1 ? t->by1 : tc->ty1;
    const int lo = t->bx0 > tc->tx0 ? t->bx0 : tc->tx0;
    const int hi = t->bx1 < tc->tx1 ? t->bx1 : tc->tx1;
    uint32_t frags = 0;
    for (int y = y0; y <= y1; y++) {
        int L, R;
        if (!row_span(t, y, lo, hi, &L, &R)) {
            continue;
        }
        uint16_t *crow = tc->cbuf + (size_t)(y - tc->ty0) * tc->cstride - tc->tx0;
        for (int x = L; x <= R; x++) {
            crow[x] = col;
        }
        frags += (uint32_t)(R - L + 1);
    }
    tc->w->fragments += frags;
}

/* ── 调试：线框标记 ───────────────────────────────────────────────── */

static void mark_wire(tile_ctx_t *tc, const gp_tri_t *t)
{
    const int y0 = t->by0 > tc->ty0 ? t->by0 : tc->ty0;
    const int y1 = t->by1 < tc->ty1 ? t->by1 : tc->ty1;
    const int lo = t->bx0 > tc->tx0 ? t->bx0 : tc->tx0;
    const int hi = t->bx1 < tc->tx1 ? t->bx1 : tc->tx1;
    float inv_len[3];
    for (int k = 0; k < 3; k++) {
        const float a = (float)t->ea[k], b = (float)t->eb[k];
        const float l = sqrtf(a * a + b * b);
        inv_len[k] = l > 0 ? 1.0f / l : 0.0f;
    }
    const int sw = tc->tg->w;
    for (int y = y0; y <= y1; y++) {
        int L, R;
        if (!row_span(t, y, lo, hi, &L, &R)) {
            continue;
        }
        for (int x = L; x <= R; x++) {
            for (int k = 0; k < 3; k++) {
                const float e = (float)((int64_t)t->ea[k] * x + (int64_t)t->eb[k] * y + t->ec[k]);
                if (e * inv_len[k] < 1.0f) {
                    tc->dbg[(size_t)y * sw + x] = 255;
                    break;
                }
            }
        }
    }
}

/* ── tile ─────────────────────────────────────────────────────────── */

static bool state_is_fast(const gp_tri_t *t, const gp_rstate_t *s)
{
    if (s->vm || s->blend || s->alpha_test || !s->color_write || (t->flags & GP_TF_FOG)) {
        return false;
    }
    if (s->shade != ESPGPU_SHADE_TEX && s->shade != ESPGPU_SHADE_TEX_MOD_COLOR) {
        return false;
    }
    if (s->shade == ESPGPU_SHADE_TEX_MOD_COLOR && !(t->flags & GP_TF_CONST_COLOR)) {
        return false;
    }
    const gp_tex_t *tex = s->smp[0].tex;
    if (tex == NULL || tex->px == NULL || s->smp[0].filter != ESPGPU_FILTER_NEAREST ||
        !(t->flags & GP_TF_TEX0) || (t->flags & GP_TF_TEX1)) {
        return false;
    }
    return t->n_att == 2;
}

static bool state_is_fill(const gp_tri_t *t, const gp_rstate_t *s)
{
    return !s->vm && !s->blend && !s->alpha_test && s->color_write && !s->depth_test &&
           !s->depth_write && s->shade == ESPGPU_SHADE_FLAT && (t->flags & GP_TF_CONST_COLOR) &&
           !(t->flags & GP_TF_FOG) && t->const_rgba[3] == 255;
}

static void raster_tile(gpu_core_t *c, int worker, int tile)
{
    gp_bin_t *b = &c->bins[tile];
    if (b->count == 0) {
        return;
    }
    gp_target_t *tg = c->target;
    gp_worker_t *w = &c->workers[worker];

    tile_ctx_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.c = c;
    tc.w = w;
    tc.tg = tg;
    const int tx = tile % c->tiles_x;
    const int ty = tile / c->tiles_x;
    tc.tx0 = tx * GP_TILE;
    tc.ty0 = ty * GP_TILE;
    tc.tx1 = tc.tx0 + GP_TILE - 1;
    tc.ty1 = tc.ty0 + GP_TILE - 1;
    if (tc.tx1 >= tg->w) tc.tx1 = tg->w - 1;
    if (tc.ty1 >= tg->h) tc.ty1 = tg->h - 1;
    const int tw = tc.tx1 - tc.tx0 + 1;
    const int th = tc.ty1 - tc.ty0 + 1;

    if (c->debug_view && c->dbg && tg == &c->screen) {
        tc.dbg = c->dbg;
        tc.dbg_mode = c->debug_view;
    }

    /* 扫一遍：是否需要深度，第一条是否整块清除 */
    bool need_depth = false;
    bool first_clears_color = false;
    bool first_clears_depth = false;
    bool first = true;
    for (uint32_t ch_off = b->head; ch_off;) {
        const gp_bin_chunk_t *ch = (const gp_bin_chunk_t *)gp_arena_ptr(c, ch_off);
        for (uint32_t i = 0; i < ch->count; i++) {
            const uint8_t *rec = gp_arena_ptr(c, ch->rec[i]);
            if (rec[0] == GP_REC_CLEAR) {
                const gp_clear_t *cl = (const gp_clear_t *)rec;
                const bool covers = cl->x0 <= tc.tx0 && cl->y0 <= tc.ty0 && cl->x1 >= tc.tx1 &&
                                    cl->y1 >= tc.ty1;
                if (first && covers) {
                    first_clears_color = (cl->flags & 1) != 0;
                    first_clears_depth = (cl->flags & 2) != 0;
                }
                if (cl->flags & 2) {
                    need_depth = true;
                }
            } else {
                const gp_tri_t *t = (const gp_tri_t *)rec;
                const gp_rstate_t *s = (const gp_rstate_t *)gp_arena_ptr(c, t->state_off);
                if (s->depth_test || s->depth_write) {
                    need_depth = true;
                }
            }
            first = false;
        }
        ch_off = ch->next;
    }
    if (need_depth && tg->depth == NULL) {
        need_depth = false; /* 分配失败时状态已被降级，不会走到这里 */
    }

    /* 颜色 */
    if (tg->color_fast) {
        tc.cbuf = tg->color + (size_t)tc.ty0 * tg->w + tc.tx0;
        tc.cstride = tg->w;
    } else {
        tc.cbuf = w->color;
        tc.cstride = GP_TILE;
        if (!first_clears_color) {
            for (int y = 0; y < th; y++) {
                memcpy(w->color + (size_t)y * GP_TILE,
                       tg->color + (size_t)(tc.ty0 + y) * tg->w + tc.tx0, (size_t)tw * 2);
            }
        }
    }
    /* 深度 */
    if (need_depth) {
        tc.dbuf = w->depth;
        if (!first_clears_depth) {
            for (int y = 0; y < th; y++) {
                memcpy(w->depth + (size_t)y * GP_TILE,
                       tg->depth + (size_t)(tc.ty0 + y) * tg->w + tc.tx0, (size_t)tw * 2);
            }
        }
    }

    for (uint32_t ch_off = b->head; ch_off;) {
        const gp_bin_chunk_t *ch = (const gp_bin_chunk_t *)gp_arena_ptr(c, ch_off);
        for (uint32_t i = 0; i < ch->count; i++) {
            const uint8_t *rec = gp_arena_ptr(c, ch->rec[i]);
            if (rec[0] == GP_REC_CLEAR) {
                const gp_clear_t *cl = (const gp_clear_t *)rec;
                const int x0 = cl->x0 > tc.tx0 ? cl->x0 : tc.tx0;
                const int y0 = cl->y0 > tc.ty0 ? cl->y0 : tc.ty0;
                const int x1 = cl->x1 < tc.tx1 ? cl->x1 : tc.tx1;
                const int y1 = cl->y1 < tc.ty1 ? cl->y1 : tc.ty1;
                for (int y = y0; y <= y1; y++) {
                    if (cl->flags & 1) {
                        uint16_t *row = tc.cbuf + (size_t)(y - tc.ty0) * tc.cstride - tc.tx0;
                        for (int x = x0; x <= x1; x++) row[x] = cl->color;
                    }
                    if ((cl->flags & 2) && tc.dbuf) {
                        uint16_t *row = tc.dbuf + (size_t)(y - tc.ty0) * GP_TILE - tc.tx0;
                        for (int x = x0; x <= x1; x++) row[x] = cl->depth;
                    }
                }
                continue;
            }
            const gp_tri_t *t = (const gp_tri_t *)rec;
            const gp_rstate_t *s = (const gp_rstate_t *)gp_arena_ptr(c, t->state_off);
            if ((s->depth_test || s->depth_write) && tc.dbuf == NULL) {
                continue;
            }
            const bool count_frags = tc.dbg && (tc.dbg_mode == ESPGPU_DEBUG_VIEW_OVERDRAW ||
                                                 tc.dbg_mode == ESPGPU_DEBUG_VIEW_TILES);
            if (!count_frags && state_is_fill(t, s)) {
                raster_tri_fill(&tc, t);
            } else if (!count_frags && state_is_fast(t, s)) {
                raster_tri_fast(&tc, t, s);
            } else {
                raster_tri_generic(&tc, t, s);
            }
            if (tc.dbg && tc.dbg_mode == ESPGPU_DEBUG_VIEW_WIREFRAME) {
                mark_wire(&tc, t);
            }
        }
        ch_off = ch->next;
    }

    if (!tg->color_fast) {
        for (int y = 0; y < th; y++) {
            memcpy(tg->color + (size_t)(tc.ty0 + y) * tg->w + tc.tx0,
                   w->color + (size_t)y * GP_TILE, (size_t)tw * 2);
        }
    }
    if (tc.dbuf) {
        for (int y = 0; y < th; y++) {
            memcpy(tg->depth + (size_t)(tc.ty0 + y) * tg->w + tc.tx0,
                   w->depth + (size_t)y * GP_TILE, (size_t)tw * 2);
        }
    }
}

static void tile_job(void *ctx, int worker)
{
    gpu_core_t *c = (gpu_core_t *)ctx;
    const int ntiles = c->tiles_x * c->tiles_y;
    for (;;) {
        const int t = gp_atomic_fetch_add(&c->next_tile, 1);
        if (t >= ntiles) {
            break;
        }
        raster_tile(c, worker, t);
    }
}

void gp_raster_frame(gpu_core_t *c)
{
    const uint64_t t0 = gp_time_us();
    c->next_tile = 0;
    for (int i = 0; i < c->nworkers; i++) {
        c->workers[i].fragments = 0;
    }
    if (c->nworkers > 1) {
        gp_parallel(tile_job, c);
    } else {
        tile_job(c, 0);
    }
    uint32_t f = 0;
    for (int i = 0; i < c->nworkers; i++) {
        f += c->workers[i].fragments;
    }
    c->last_fill += f;
    c->st_raster_us += (uint32_t)(gp_time_us() - t0);
}

/* ── 调试画面合成 ─────────────────────────────────────────────────── */

static uint16_t heat(uint32_t v, uint32_t vmax)
{
    /* 黑 → 蓝 → 青 → 黄 → 红 */
    float t = vmax ? (float)v / (float)vmax : 0.0f;
    if (t > 1.0f) t = 1.0f;
    uint32_t r, g, b;
    if (t < 0.25f) {
        r = 0; g = 0; b = (uint32_t)(t * 4.0f * 255.0f);
    } else if (t < 0.5f) {
        r = 0; g = (uint32_t)((t - 0.25f) * 4.0f * 255.0f); b = 255;
    } else if (t < 0.75f) {
        r = (uint32_t)((t - 0.5f) * 4.0f * 255.0f); g = 255; b = (uint32_t)((0.75f - t) * 4.0f * 255.0f);
    } else {
        r = 255; g = (uint32_t)((1.0f - t) * 4.0f * 255.0f); b = 0;
    }
    return gp_pack565(r, g, b);
}

void gp_compose_debug(gpu_core_t *c)
{
    if (c->dbg_img == NULL) {
        return;
    }
    const gp_target_t *s = &c->screen;
    const int W = s->w, H = s->h;
    const size_t n = (size_t)W * H;

    switch (c->debug_view) {
    case ESPGPU_DEBUG_VIEW_DEPTH:
        for (size_t i = 0; i < n; i++) {
            const uint32_t d = s->depth ? s->depth[i] : 0xFFFF;
            /* 近处亮：取反并做简单伽马，让近距离细节可见 */
            const float f = 1.0f - (float)d / 65535.0f;
            const uint32_t v = (uint32_t)(sqrtf(f) * 255.0f);
            c->dbg_img[i] = gp_pack565(v, v, v);
        }
        break;
    case ESPGPU_DEBUG_VIEW_OVERDRAW:
        for (size_t i = 0; i < n; i++) {
            c->dbg_img[i] = heat(c->dbg ? c->dbg[i] : 0, 8);
        }
        break;
    case ESPGPU_DEBUG_VIEW_TILES: {
        /* 原画面压暗 + 每 tile 三角形数热力 + 网格 */
        uint32_t maxc = 1;
        for (int t = 0; t < c->tiles_x * c->tiles_y; t++) {
            if (c->bins[t].count > maxc) maxc = c->bins[t].count;
        }
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const size_t i = (size_t)y * W + x;
                const uint32_t src = gp_expand565(s->color[i]);
                uint32_t r = ((src >> 16) & 255) / 3, g = ((src >> 8) & 255) / 3, b = (src & 255) / 3;
                if ((x % GP_TILE) == 0 || (y % GP_TILE) == 0) {
                    r = g = b = 200;
                }
                c->dbg_img[i] = gp_pack565(r, g, b);
            }
        }
        /* 分箱在 PRESENT 前已经清空，这里用帧统计：每 tile 填充像素的近似由 overdraw 缓冲给出 */
        if (c->dbg) {
            for (int ty = 0; ty < c->tiles_y; ty++) {
                for (int tx = 0; tx < c->tiles_x; tx++) {
                    uint32_t sum = 0;
                    const int x0 = tx * GP_TILE, y0 = ty * GP_TILE;
                    const int x1 = x0 + GP_TILE < W ? x0 + GP_TILE : W;
                    const int y1 = y0 + GP_TILE < H ? y0 + GP_TILE : H;
                    for (int y = y0; y < y1; y++) {
                        for (int x = x0; x < x1; x++) {
                            sum += c->dbg[(size_t)y * W + x];
                        }
                    }
                    const uint32_t area = (uint32_t)((x1 - x0) * (y1 - y0));
                    const uint16_t hc = heat(area ? sum * 4 / area : 0, 8);
                    for (int y = y0 + 2; y < y0 + 8 && y < y1; y++) {
                        for (int x = x0 + 2; x < x1 - 2; x++) {
                            c->dbg_img[(size_t)y * W + x] = hc;
                        }
                    }
                }
            }
        }
        (void)maxc;
        break;
    }
    case ESPGPU_DEBUG_VIEW_WIREFRAME:
        for (size_t i = 0; i < n; i++) {
            if (c->dbg && c->dbg[i]) {
                c->dbg_img[i] = gp_pack565(80, 255, 120);
            } else {
                const uint32_t src = gp_expand565(s->color[i]);
                c->dbg_img[i] = gp_pack565(((src >> 16) & 255) / 4, ((src >> 8) & 255) / 4,
                                           (src & 255) / 4);
            }
        }
        break;
    default:
        memcpy(c->dbg_img, s->color, n * 2);
        break;
    }
}
