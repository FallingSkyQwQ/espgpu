/*
 * raster3d.c — tile 光栅器
 *
 * PRESENT（或 arena 满、纹理被改、FBO 切换）时逐 tile 执行：
 *   1. 取 tile 的颜色 / 深度到内部 SRAM（屏幕目标本来就在 SRAM 时颜色直接写）
 *   2. 依序执行分箱记录：CLEAR 矩形、三角形
 *   3. 写回
 *
 * 三角形按行求跨度（边函数解析求交，不逐像素做内测试；tile 内整体在内侧的边不再逐行看），
 * 跨度内每 8 像素做一次精确透视除法、中间线性插值（经典的 Quake 式分段透视校正）；
 * 段终点即下一段起点，1/w 每段只除一次，最后一段终点取跨度末像素（不外推到三角形外）。
 *
 * 内层循环按"状态组合"分三条：
 *   fill   —— 常量色（含无纹理的 TEX_MOD_COLOR）、不混合、无深度：UI / 背景 / 2D 纯色块
 *   fast   —— 单纹理（RGB565）× 常量色、最近邻、不混合、无 alpha 测试 / 雾，可带深度：
 *             2D 背景与 Doom 墙面地面的主路径。纹理坐标 16.16 定点步进，
 *             REPEAT + 2 的幂尺寸时 texel 下标直接取掩码
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
    uint8_t *dbg;             /* 屏幕目标的调试缓冲，可空 */
    uint8_t dbg_mode;
} tile_ctx_t;

/* ── 混合 ─────────────────────────────────────────────────────────── */

/**
 * 混合因子的无分支形式：f = base ± sel，sel ∈ {0, 源色, 目标色, 源 alpha}。
 * 每个三角形算一次，内层循环里不再对因子 switch（取值与 GL 因子定义逐一相同）。
 */
typedef struct {
    uint32_t base, msc, mdc, msa, neg;
} bfac_t;

static void bfac_init(bfac_t *f, uint8_t factor)
{
    f->base = 0;
    f->msc = f->mdc = f->msa = 0;
    f->neg = 0;
    switch (factor) {
    case ESPGPU_BLEND_ZERO: break;
    case ESPGPU_BLEND_SRC_COLOR: f->msc = ~0u; break;
    case ESPGPU_BLEND_ONE_MINUS_SRC_COLOR: f->base = 255; f->msc = ~0u; f->neg = ~0u; break;
    case ESPGPU_BLEND_DST_COLOR: f->mdc = ~0u; break;
    case ESPGPU_BLEND_ONE_MINUS_DST_COLOR: f->base = 255; f->mdc = ~0u; f->neg = ~0u; break;
    case ESPGPU_BLEND_SRC_ALPHA: f->msa = ~0u; break;
    case ESPGPU_BLEND_ONE_MINUS_SRC_ALPHA: f->base = 255; f->msa = ~0u; f->neg = ~0u; break;
    case ESPGPU_BLEND_ONE_MINUS_DST_ALPHA: break;      /* 帧缓冲无 alpha：1 - 1 = 0 */
    default: f->base = 255; break;                     /* ONE / DST_ALPHA / 未知 */
    }
}

static inline uint32_t bfac(const bfac_t *f, uint32_t sc, uint32_t dc, uint32_t sa)
{
    const uint32_t sel = (sc & f->msc) | (dc & f->mdc) | (sa & f->msa);
    return f->base + ((sel ^ f->neg) - f->neg);
}

typedef struct {
    bfac_t src, dst;
    uint8_t op;
} blend_t;

static inline uint32_t blend_ch(const blend_t *b, uint32_t sc, uint32_t dc, uint32_t sa)
{
    const uint32_t x = gp_mul8(sc, bfac(&b->src, sc, dc, sa));
    const uint32_t y = gp_mul8(dc, bfac(&b->dst, sc, dc, sa));
    int32_t r;
    switch (b->op) {
    case ESPGPU_BLEND_OP_SUB: r = (int32_t)x - (int32_t)y; break;
    case ESPGPU_BLEND_OP_REV_SUB: r = (int32_t)y - (int32_t)x; break;
    case ESPGPU_BLEND_OP_MIN: r = (int32_t)(sc < dc ? sc : dc); break;
    case ESPGPU_BLEND_OP_MAX: r = (int32_t)(sc > dc ? sc : dc); break;
    default: r = (int32_t)(x + y); break;
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

/** 夹到 [0,1]；NaN（VM 输出 / 病态插值）当 0，免得后面的 float→int 是 UB */
static inline float unit_f(float v)
{
    return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
}

static inline uint32_t float_rgba(const float *v)
{
    const int r = (int)(unit_f(v[0]) * 255.0f + 0.5f);
    const int g = (int)(unit_f(v[1]) * 255.0f + 0.5f);
    const int b = (int)(unit_f(v[2]) * 255.0f + 0.5f);
    const int a = (int)(unit_f(v[3]) * 255.0f + 0.5f);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/** 深度 float → 16 位：范围内与 clamp((int)z) 相同，超范围 / NaN 不触发 float→int 的 UB */
static inline uint32_t z16(float z)
{
    if (!(z > 0.0f)) return 0;
    if (z >= 65535.0f) return 65535;
    return (uint32_t)z;
}

/* ── 跨度求解 ─────────────────────────────────────────────────────── */

/**
 * 一个三角形在一个 tile 矩形 [lo,hi]×[y0,y1] 内的逐行跨度。
 *
 * 初始化时把三条边分成三类：整个矩形都在内侧（以后不再看）、整个在外侧（整块跳过）、
 * 穿过矩形（"活跃"）。逐行只处理活跃边：E(x=0) 每行加 b（int64 加法），起点估计
 * est = -E/a 每行加一个常量（float 加法），再用精确的整数边函数修正 ±1。
 * 这样每行不再有 int64→float 转换、ceilf/floorf 库调用；结果与逐行解析求解逐像素相同。
 */
typedef struct {
    int n;
    int lo, hi;
    int32_t a[3];
    int32_t b[3];
    int64_t E[3];     /* 当前行 x=0 处的边函数值 */
    float est[3];     /* 当前行的交点估计 -E/a */
    float estep[3];
} span_t;

GP_HOT static inline bool span_init(span_t *sp, const gp_tri_t *t, int lo, int hi, int y0, int y1)
{
    sp->n = 0;
    sp->lo = lo;
    sp->hi = hi;
    if (lo > hi || y0 > y1) {
        return false;
    }
    const int64_t rw = hi - lo, rh = y1 - y0;
    for (int k = 0; k < 3; k++) {
        const int64_t a = t->ea[k], b = t->eb[k];
        const int64_t e00 = a * lo + b * y0 + t->ec[k];
        const int64_t emin = e00 + (a < 0 ? a * rw : 0) + (b < 0 ? b * rh : 0);
        const int64_t emax = e00 + (a > 0 ? a * rw : 0) + (b > 0 ? b * rh : 0);
        if (emax < 0) {
            return false; /* 整个矩形在这条边外侧 */
        }
        if (emin >= 0) {
            continue;     /* 整个矩形在内侧 */
        }
        const int i = sp->n++;
        sp->a[i] = t->ea[k];
        sp->b[i] = t->eb[k];
        sp->E[i] = b * y0 + t->ec[k];
        sp->est[i] = a != 0 ? -(float)sp->E[i] * t->inv_ea[k] : 0.0f;
        sp->estep[i] = a != 0 ? -(float)b * t->inv_ea[k] : 0.0f;
    }
    return true;
}

/** 当前行的跨度（空则返回 false），并前进到下一行。必须对 y0..y1 每一行依次调用。 */
GP_HOT static inline bool span_next(span_t *sp, int *out_l, int *out_r)
{
    const int lo = sp->lo, hi = sp->hi;
    int L = lo, R = hi;
    bool ok = true;
    for (int k = 0; k < sp->n; k++) {
        const int64_t a = sp->a[k];
        const int64_t Ey = sp->E[k];
        float est = sp->est[k];
        sp->E[k] = Ey + sp->b[k];
        sp->est[k] = est + sp->estep[k];
        if (!ok) {
            continue;
        }
        if (a == 0) {
            ok = Ey >= 0;
            continue;
        }
        if (!(est > (float)(lo - 1))) est = (float)(lo - 1);
        if (est > (float)(hi + 1)) est = (float)(hi + 1);
        int x = (int)est;
        if (a > 0) {
            while (x <= hi && a * x + Ey < 0) x++;
            while (x > lo && a * (x - 1) + Ey >= 0) x--;
            if (x > L) L = x;
        } else {
            while (x >= lo && a * x + Ey < 0) x--;
            while (x < hi && a * (x + 1) + Ey >= 0) x++;
            if (x < R) R = x;
        }
        ok = L <= R;
    }
    *out_l = L;
    *out_r = R;
    return ok;
}

static inline float plane_at(const gp_plane_t *p, float x, float y)
{
    return p->c + p->dx * x + p->dy * y;
}

/** 一行的深度起点：起点 / 终点超出 ±1e9（病态平面）时把这一行压平，避免累加出 inf */
GP_HOT static inline void row_z(const gp_tri_t *t, int L, int R, float fy, float *z, float *dz)
{
    *z = plane_at(&t->z, (float)L, fy);
    *dz = t->z.dx;
    if (!(fabsf(*z) < 1e9f) || !(fabsf(*dz) * (float)(R - L + 1) < 1e9f)) {
        *z = gp_clampf(*z, 0.0f, 65535.0f); /* NaN 时 clampf 原样返回，z16 再兜底 */
        *dz = 0.0f;
    }
}

/* 1/n，n = 0..SEG（常量折叠，与运行时 1.0f / n 逐位相同） */
static const float k_inv[SEG + 1] = {0.0f, 1.0f / 1, 1.0f / 2, 1.0f / 3, 1.0f / 4,
                                     1.0f / 5, 1.0f / 6, 1.0f / 7, 1.0f / 8};

/* ── 通用路径 ─────────────────────────────────────────────────────── */

GP_HOT static void raster_tri_generic(tile_ctx_t *tc, const gp_tri_t *t, const gp_rstate_t *s)
{
    const int n = t->n_att;
    const bool affine = (t->flags & GP_TF_AFFINE) != 0;
    const bool vm = (t->flags & GP_TF_VM) != 0 && s->vm && s->prog;

    const int o_col = t->o_col, o_t0 = t->o_t0, o_t1 = t->o_t1, o_fog = t->o_fog;

    const uint32_t Cconst = ((uint32_t)t->const_rgba[3] << 24) | ((uint32_t)t->const_rgba[0] << 16) |
                            ((uint32_t)t->const_rgba[1] << 8) | t->const_rgba[2];
    const bool dtest = s->depth_test != 0;
    const bool dwrite = s->depth_write != 0;
    const uint32_t zmask = gp_cmp_mask(s->depth_func);
    const bool cwrite = s->color_write != 0;
    const bool blend = s->blend != 0;
    blend_t bl;
    bfac_init(&bl.src, s->bsrc);
    bfac_init(&bl.dst, s->bdst);
    bl.op = s->bop;
    const bool atest = s->alpha_test != 0;
    const uint32_t amask = gp_cmp_mask(s->alpha_func);
    const uint32_t aref = s->alpha_ref;
    const bool fog = s->fog && o_fog >= 0;
    const uint32_t fog_r = s->fog_r, fog_g = s->fog_g, fog_b = s->fog_b;
    const gp_sampler_t *s0 = (s->smp[0].tex && o_t0 >= 0) ? &s->smp[0] : NULL;
    const gp_sampler_t *s1 = (s->smp[1].tex && o_t1 >= 0) ? &s->smp[1] : NULL;
    const bool lin0 = s0 && s0->filter == ESPGPU_FILTER_LINEAR;
    const bool lin1 = s1 && s1->filter == ESPGPU_FILTER_LINEAR;
    const uint8_t mode = s->shade;
    uint8_t *dbg = tc->dbg_mode == ESPGPU_DEBUG_VIEW_OVERDRAW ? tc->dbg : NULL;
    const int sw = tc->tg->w;

    const int y0 = t->by0 > tc->ty0 ? t->by0 : tc->ty0;
    const int y1 = t->by1 < tc->ty1 ? t->by1 : tc->ty1;
    const int lo = t->bx0 > tc->tx0 ? t->bx0 : tc->tx0;
    const int hi = t->bx1 < tc->tx1 ? t->bx1 : tc->tx1;

    span_t sp;
    if (!span_init(&sp, t, lo, hi, y0, y1)) {
        return;
    }

    float va[GP_MAX_ATT];
    float vstep[GP_MAX_ATT];
    float ve[GP_MAX_ATT];   /* 段终点的属性值，下一段直接接着用 */
    float vmv[ESPGPU_VM_VARYINGS * 4];
    const int nv = (o_fog >= 0 ? n - 1 : n); /* VM：雾因子在最后，不是 varying */
    if (vm) {
        memset(vmv, 0, sizeof(vmv)); /* 未用到的 varying 恒为 0，只需清一次 */
    }
    uint32_t frags = 0;

    for (int y = y0; y <= y1; y++) {
        int L, R;
        if (!span_next(&sp, &L, &R)) {
            continue;
        }
        uint16_t *crow = tc->cbuf + (size_t)(y - tc->ty0) * tc->cstride - tc->tx0;
        uint16_t *drow = tc->dbuf ? tc->dbuf + (size_t)(y - tc->ty0) * GP_TILE - tc->tx0 : NULL;
        const float fy = (float)y;
        float z, dz;
        row_z(t, L, R, fy, &z, &dz);
        bool have_end = false;

        for (int xs = L; xs <= R; xs += SEG) {
            const int len = (R - xs + 1) < SEG ? (R - xs + 1) : SEG;
            const float fx = (float)xs;
            if (affine) {
                for (int k = 0; k < n; k++) {
                    va[k] = plane_at(&t->att[k], fx, fy);
                    vstep[k] = t->att[k].dx;
                }
            } else {
                if (have_end) {
                    for (int k = 0; k < n; k++) {
                        va[k] = ve[k];
                    }
                } else {
                    const float iw_s = plane_at(&t->iw, fx, fy);
                    const float ws = iw_s != 0.0f ? 1.0f / iw_s : 0.0f;
                    for (int k = 0; k < n; k++) {
                        va[k] = plane_at(&t->att[k], fx, fy) * ws;
                    }
                }
                /* 段终点：中间段取下一段起点（可复用，1/w 每段只除一次）；
                 * 最后一段取 R 本身——R+1 在三角形外，1/w 外推可能接近 0 或变负，把整段插坏 */
                const int steps = (xs + SEG <= R) ? SEG : len - 1;
                if (steps > 0) {
                    const float fe = fx + (float)steps;
                    const float iw_e = plane_at(&t->iw, fe, fy);
                    const float we = iw_e != 0.0f ? 1.0f / iw_e : 0.0f;
                    const float inv_len = k_inv[steps];
                    for (int k = 0; k < n; k++) {
                        const float ae = plane_at(&t->att[k], fe, fy) * we;
                        vstep[k] = (ae - va[k]) * inv_len;
                        ve[k] = ae;
                    }
                } else {
                    for (int k = 0; k < n; k++) {
                        vstep[k] = 0.0f;
                    }
                }
                have_end = true;
            }

            for (int i = 0; i < len; i++, z += dz) {
                const int x = xs + i;
                uint32_t zi = 0;
                if (dtest || dwrite) {
                    zi = z16(z);
                }
                if (dtest && !gp_cmp_m(zmask, zi, drow[x])) {
                    goto next;
                }

                uint32_t out;
                if (vm) {
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
                    uint32_t T0 = 0xFFFFFFFFu, T1 = 0xFFFFFFFFu;
                    if (s0) {
                        T0 = lin0 ? gp_sample_linear(s0, va[o_t0], va[o_t0 + 1])
                                  : gp_sample_nearest(s0, va[o_t0], va[o_t0 + 1]);
                    }
                    if (s1) {
                        T1 = lin1 ? gp_sample_linear(s1, va[o_t1], va[o_t1 + 1])
                                  : gp_sample_nearest(s1, va[o_t1], va[o_t1 + 1]);
                    }
                    out = shade_fixed(mode, C, T0, T1);
                }

                if (fog) {
                    const uint32_t f = (uint32_t)(unit_f(va[o_fog]) * 255.0f + 0.5f);
                    const uint32_t r = gp_mul8((out >> 16) & 255, f) + gp_mul8(fog_r, 255 - f);
                    const uint32_t g = gp_mul8((out >> 8) & 255, f) + gp_mul8(fog_g, 255 - f);
                    const uint32_t b = gp_mul8(out & 255, f) + gp_mul8(fog_b, 255 - f);
                    out = (out & 0xFF000000u) | ((r > 255 ? 255 : r) << 16) |
                          ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
                }

                if (atest && !gp_cmp_m(amask, out >> 24, aref)) {
                    goto next;
                }

                if (cwrite) {
                    if (blend) {
                        const uint32_t d = gp_expand565(crow[x]);
                        const uint32_t sa = out >> 24;
                        const uint32_t r = blend_ch(&bl, (out >> 16) & 255, (d >> 16) & 255, sa);
                        const uint32_t g = blend_ch(&bl, (out >> 8) & 255, (d >> 8) & 255, sa);
                        const uint32_t b = blend_ch(&bl, out & 255, d & 255, sa);
                        crow[x] = gp_pack565(r, g, b);
                    } else {
                        crow[x] = gp_pack565((out >> 16) & 255, (out >> 8) & 255, out & 255);
                    }
                }
                if (dwrite) {
                    drow[x] = (uint16_t)zi;
                }
                frags++;
                if (dbg) {
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
}

/* ── 快速路径：RGB565 纹理 × 常量色、最近邻、不混合 ────────────── */

GP_HOT static void raster_tri_fast(tile_ctx_t *tc, const gp_tri_t *t, const gp_rstate_t *s)
{
    const gp_sampler_t *sm = &s->smp[0];
    const gp_tex_t *tex = sm->tex;
    const uint16_t *tpx = tex->px;
    const int tw = tex->w, th = tex->h;
    const bool rep_u = sm->wrap_u != ESPGPU_WRAP_CLAMP;
    const bool rep_v = sm->wrap_v != ESPGPU_WRAP_CLAMP;
    /* 起点折回的周期：REPEAT = n，MIRROR = 2n */
    const float fpu = (float)(sm->wrap_u == ESPGPU_WRAP_MIRROR ? 2 * tw : tw);
    const float fpv = (float)(sm->wrap_v == ESPGPU_WRAP_MIRROR ? 2 * th : th);
    /* 两个方向都是 REPEAT 且尺寸是 2 的幂：texel 下标用掩码 */
    const bool pow2 = sm->wrap_u == ESPGPU_WRAP_REPEAT && sm->wrap_v == ESPGPU_WRAP_REPEAT &&
                      (tw & (tw - 1)) == 0 && (th & (th - 1)) == 0;
    const uint32_t mask_u = (uint32_t)tw - 1u, mask_v = (uint32_t)th - 1u;
    int shift_w = 0;
    while ((1 << shift_w) < tw) {
        shift_w++;
    }
    const bool affine = (t->flags & GP_TF_AFFINE) != 0;
    const bool dtest = s->depth_test != 0;
    const bool dwrite = s->depth_write != 0;
    const uint32_t zmask = gp_cmp_mask(s->depth_func);

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

    span_t sp;
    if (!span_init(&sp, t, lo, hi, y0, y1)) {
        return;
    }

    for (int y = y0; y <= y1; y++) {
        int L, R;
        if (!span_next(&sp, &L, &R)) {
            continue;
        }
        uint16_t *crow = tc->cbuf + (size_t)(y - tc->ty0) * tc->cstride - tc->tx0;
        uint16_t *drow = tc->dbuf ? tc->dbuf + (size_t)(y - tc->ty0) * GP_TILE - tc->tx0 : NULL;
        const float fy = (float)y;
        float z, dz;
        row_z(t, L, R, fy, &z, &dz);
        float ue = 0.0f, ve = 0.0f;
        bool have_end = false;

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
                if (have_end) {
                    u = ue;
                    v = ve;
                } else {
                    const float iw_s = plane_at(&t->iw, fx, fy);
                    const float ws = iw_s != 0.0f ? 1.0f / iw_s : 0.0f;
                    u = plane_at(pu, fx, fy) * ws;
                    v = plane_at(pv, fx, fy) * ws;
                }
                /* 段终点：中间段取下一段起点（可复用，1/w 每段只除一次）；
                 * 最后一段取 R 本身——R+1 在三角形外，1/w 外推可能接近 0 或变负，把整段插坏 */
                const int steps = (xs + SEG <= R) ? SEG : len - 1;
                if (steps > 0) {
                    const float fe = fx + (float)steps;
                    const float iw_e = plane_at(&t->iw, fe, fy);
                    const float we = iw_e != 0.0f ? 1.0f / iw_e : 0.0f;
                    const float inv_len = k_inv[steps];
                    const float pue = plane_at(pu, fe, fy);
                    const float pve = plane_at(pv, fe, fy);
                    du = (pue * we - u) * inv_len;
                    dv = (pve * we - v) * inv_len;
                    ue = pue * we;
                    ve = pve * we;
                } else {
                    du = dv = 0.0f;
                }
                have_end = true;
            }
            /* 折回起点：REPEAT 到 [0, n)，MIRROR 到 [0, 2n)（周期不变）。
             * 定点步进要求坐标幅度有限，折回后通常只剩 [-n, 2n) 量级 */
            if (rep_u && (u < 0.0f || u >= fpu)) {
                u -= floorf(u / fpu) * fpu;
            }
            if (rep_v && (v < 0.0f || v >= fpv)) {
                v -= floorf(v / fpv) * fpv;
            }
            const float uz = u + du * (float)(len - 1);
            const float vz = v + dv * (float)(len - 1);

            if (fabsf(u) < 16000.0f && fabsf(uz) < 16000.0f && fabsf(v) < 16000.0f &&
                fabsf(vz) < 16000.0f) {
                /* 16.16 定点步进：每像素两次整数加法 + 移位，省掉 float→int、float 比较。
                 * 段两端都在 ±16000 内，中间各点（线性）也在，int32 不会溢出。
                 * 负数右移按算术移位（GCC/Clang/MSVC 都是），即 floor。 */
                /* 四舍五入到 1/65536：截断会让恰好落在 texel 边界上的坐标（2D 里很常见）掉到前一个 texel */
                int32_t U = gp_floor_i(u * 65536.0f + 0.5f), V = gp_floor_i(v * 65536.0f + 0.5f);
                const int32_t dU = len > 1 ? gp_floor_i(du * 65536.0f + 0.5f) : 0;
                const int32_t dV = len > 1 ? gp_floor_i(dv * 65536.0f + 0.5f) : 0;
                if (pow2) {
                    /* REPEAT + 2 的幂：取模就是与掩码，无分支 */
                    for (int i = 0; i < len; i++, z += dz, U += dU, V += dV) {
                        const int x = xs + i;
                        uint32_t zi = 0;
                        if (dtest || dwrite) {
                            zi = z16(z);
                            if (dtest && !gp_cmp_m(zmask, zi, drow[x])) {
                                continue;
                            }
                        }
                        const uint32_t iu = (uint32_t)(U >> 16) & mask_u;
                        const uint32_t iv = (uint32_t)(V >> 16) & mask_v;
                        uint16_t px = tpx[(iv << shift_w) | iu];
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
                } else {
                    for (int i = 0; i < len; i++, z += dz, U += dU, V += dV) {
                        const int x = xs + i;
                        uint32_t zi = 0;
                        if (dtest || dwrite) {
                            zi = z16(z);
                            if (dtest && !gp_cmp_m(zmask, zi, drow[x])) {
                                continue;
                            }
                        }
                        int iu = (int)(U >> 16);
                        int iv = (int)(V >> 16);
                        if ((unsigned)iu >= (unsigned)tw) {
                            iu = gp_wrap(iu, tw, sm->wrap_u);
                        }
                        if ((unsigned)iv >= (unsigned)th) {
                            iv = gp_wrap(iv, th, sm->wrap_v);
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
                continue;
            }

            /* 少见的大坐标（CLAMP 下纹理坐标远超纹理、极端缩小）：逐像素 float，
             * 病态值（|u| 上亿 / NaN）钉在 0，免得 float→int 越界 */
            if (!(fabsf(u) < 1e8f) || !(fabsf(du) < 1e7f)) {
                u = 0.0f;
                du = 0.0f;
            }
            if (!(fabsf(v) < 1e8f) || !(fabsf(dv) < 1e7f)) {
                v = 0.0f;
                dv = 0.0f;
            }
            for (int i = 0; i < len; i++, z += dz, u += du, v += dv) {
                const int x = xs + i;
                uint32_t zi = 0;
                if (dtest || dwrite) {
                    zi = z16(z);
                    if (dtest && !gp_cmp_m(zmask, zi, drow[x])) {
                        continue;
                    }
                }
                const int iu = gp_wrap(gp_floor_i(u), tw, sm->wrap_u);
                const int iv = gp_wrap(gp_floor_i(v), th, sm->wrap_v);
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
    span_t sp;
    if (!span_init(&sp, t, lo, hi, y0, y1)) {
        return;
    }
    uint32_t frags = 0;
    for (int y = y0; y <= y1; y++) {
        int L, R;
        if (!span_next(&sp, &L, &R)) {
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
    span_t sp;
    if (!span_init(&sp, t, lo, hi, y0, y1)) {
        return;
    }
    const int sw = tc->tg->w;
    for (int y = y0; y <= y1; y++) {
        int L, R;
        if (!span_next(&sp, &L, &R)) {
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
    if (s->vm || s->blend || s->alpha_test || !s->color_write || s->depth_test || s->depth_write ||
        !(t->flags & GP_TF_CONST_COLOR) || (t->flags & GP_TF_FOG)) {
        return false;
    }
    /* 不混合、无 alpha 测试时 alpha 不影响结果。TEX_MOD_COLOR 没有纹理时 T0 = 白，
     * 白 × C = C（gp_mul8(255, c) == c），于是 2D 的纯色四边形也走填充 */
    if (s->shade == ESPGPU_SHADE_FLAT) {
        return true;
    }
    return s->shade == ESPGPU_SHADE_TEX_MOD_COLOR &&
           (s->smp[0].tex == NULL || !(t->flags & GP_TF_TEX0) || t->o_t0 < 0);
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

    /* 扫一遍：是否需要深度、是否会写深度，第一条是否整块清除 */
    bool need_depth = false;
    bool writes_depth = false;
    uint32_t ntris = 0;
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
                    writes_depth = true;
                }
            } else {
                const gp_tri_t *t = (const gp_tri_t *)rec;
                const gp_rstate_t *s = (const gp_rstate_t *)gp_arena_ptr(c, t->state_off);
                if (s->depth_test || s->depth_write) {
                    need_depth = true;
                }
                if (s->depth_write) {
                    writes_depth = true;
                }
                ntris++;
            }
            first = false;
        }
        ch_off = ch->next;
    }
    if (need_depth && tg->depth == NULL) {
        need_depth = false; /* 分配失败时状态已被降级，不会走到这里 */
    }
    if (tc.dbg && tc.dbg_mode == ESPGPU_DEBUG_VIEW_TILES) {
        c->dbg_tile_tris[tile] += ntris; /* 每个 tile 只属于一个 worker，不用加锁 */
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
            /* OVERDRAW 要逐片元计数，只有通用路径做这件事 */
            const bool count_frags = tc.dbg && tc.dbg_mode == ESPGPU_DEBUG_VIEW_OVERDRAW;
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
    /* 深度只测不写（或全被跳过）时 PSRAM 里的副本没变，省掉写回 */
    if (tc.dbuf && writes_depth) {
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
        /* 原画面压暗 + 网格 + 每 tile 顶部一条热力条：本帧光栅到该 tile 的三角形数
         * （按全帧最大值归一化）。计数在光栅时累加，PRESENT 前分箱已经清空，不能再看 bins */
        uint32_t maxc = 1;
        for (int t = 0; t < c->tiles_x * c->tiles_y; t++) {
            if (c->dbg_tile_tris[t] > maxc) maxc = c->dbg_tile_tris[t];
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
        for (int ty = 0; ty < c->tiles_y; ty++) {
            for (int tx = 0; tx < c->tiles_x; tx++) {
                const uint32_t cnt = c->dbg_tile_tris[ty * c->tiles_x + tx];
                if (cnt == 0) {
                    continue;
                }
                const uint16_t hc = heat(cnt, maxc);
                const int x0 = tx * GP_TILE, y0 = ty * GP_TILE;
                const int x1 = x0 + GP_TILE < W ? x0 + GP_TILE : W;
                const int y1 = y0 + GP_TILE < H ? y0 + GP_TILE : H;
                for (int y = y0 + 2; y < y0 + 8 && y < y1; y++) {
                    for (int x = x0 + 2; x < x1 - 2; x++) {
                        c->dbg_img[(size_t)y * W + x] = hc;
                    }
                }
            }
        }
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
