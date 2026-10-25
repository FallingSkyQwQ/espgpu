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
 * 内层循环按"状态组合"分四条：
 *   fill   —— 常量色（含无纹理的 TEX_MOD_COLOR）、不混合、无深度：UI / 背景 / 2D 纯色块
 *   fast   —— 单纹理（RGB565 或不透明调色板 INDEX8）× 常量色、最近邻、不混合、无 alpha 测试 / 雾，
 *             可带深度：2D 背景与 Doom 墙面地面的主路径。纹理坐标 16.16 定点步进，
 *             REPEAT + 2 的幂尺寸时 texel 下标直接取掩码
 *   px     —— 像素管线定点路径：单纹理（任意格式，最近邻 / 双线性）× 插值或常量色、雾、
 *             深度、alpha 测试、常见混合：光照 + 雾的 3D 场景、精灵、半透明（见下面的说明）
 *   generic—— 其它一切（多纹理、VM、特殊混合 / 着色模式、越界顶点色），逐像素浮点
 *
 * 深度在所有路径里都按 row_zfix 的同一套定点算法求值：同一几何多遍绘制（EQUAL / LEQUAL）
 * 不管各遍走哪条路径，逐像素深度都一致。
 */

#include <math.h>

#include "core_internal.h"

#define SEG 8
/* px 路径（Doom 这类批量小三角形）的段长单独一份：段建立的固定开销按段摊，
 * Doom 320x200 一行平均只有 ~7 像素、两段，16 像素一段能省掉一半的段建立。
 * 通用 / fast 路径仍是 SEG（VM 的 span 缓冲 GP_VM_SPAN 与 SEG 对应，不能跟着变）。 */
#define PX_SEG 16

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

/**
 * 比较函数（ESPGPU_CMP_*）→ 一次无符号比较：pass ⇔ (uint32_t)((a ^ x) - (b ^ x) + o) < lim，
 * a、b ∈ [0, 65535]。x = ~0 时 (~a) - (~b) = b - a。
 */
typedef struct {
    uint32_t x, o, lim;
} cmpf_t;

static void cmpf_init(cmpf_t *f, uint8_t func)
{
    f->x = 0;
    f->o = 0x10000u;
    switch (func) {
    case ESPGPU_CMP_NEVER: f->lim = 0; break;
    case ESPGPU_CMP_LESS: f->lim = 0x10000u; break;          /* a - b < 0 */
    case ESPGPU_CMP_LEQUAL: f->lim = 0x10001u; break;        /* a - b ≤ 0 */
    case ESPGPU_CMP_GREATER: f->x = ~0u; f->lim = 0x10000u; break;
    case ESPGPU_CMP_GEQUAL: f->x = ~0u; f->lim = 0x10001u; break;
    case ESPGPU_CMP_EQUAL: f->o = 0; f->lim = 1; break;      /* a - b == 0 */
    case ESPGPU_CMP_NOTEQUAL: f->o = ~0u; f->lim = ~0u; break; /* a - b - 1 ≠ -1 */
    default: f->lim = 0x20000u; break;                      /* ALWAYS / 未知（同 gp_cmp_mask） */
    }
}

static inline bool cmpf(const cmpf_t *f, uint32_t a, uint32_t b)
{
    return ((a ^ f->x) - (b ^ f->x) + f->o) < f->lim;
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

/* 1/n，n = 0..GP_TILE（行深度步进；常量折叠，与运行时 1.0f / n 逐位相同） */
static const float k_inv_row[GP_TILE + 1] = {
    0.0f, 1.0f / 1, 1.0f / 2, 1.0f / 3, 1.0f / 4, 1.0f / 5, 1.0f / 6, 1.0f / 7,
    1.0f / 8, 1.0f / 9, 1.0f / 10, 1.0f / 11, 1.0f / 12, 1.0f / 13, 1.0f / 14, 1.0f / 15,
    1.0f / 16, 1.0f / 17, 1.0f / 18, 1.0f / 19, 1.0f / 20, 1.0f / 21, 1.0f / 22, 1.0f / 23,
    1.0f / 24, 1.0f / 25, 1.0f / 26, 1.0f / 27, 1.0f / 28, 1.0f / 29, 1.0f / 30, 1.0f / 31,
    1.0f / 32, 1.0f / 33, 1.0f / 34, 1.0f / 35, 1.0f / 36, 1.0f / 37, 1.0f / 38, 1.0f / 39,
    1.0f / 40, 1.0f / 41, 1.0f / 42, 1.0f / 43, 1.0f / 44, 1.0f / 45, 1.0f / 46, 1.0f / 47,
    1.0f / 48, 1.0f / 49, 1.0f / 50, 1.0f / 51, 1.0f / 52, 1.0f / 53, 1.0f / 54, 1.0f / 55,
    1.0f / 56, 1.0f / 57, 1.0f / 58, 1.0f / 59, 1.0f / 60, 1.0f / 61, 1.0f / 62, 1.0f / 63,
    1.0f / 64,
};

static inline float zclamp(float z)
{
    return z > 0.0f ? (z < 65535.0f ? z : 65535.0f) : 0.0f; /* NaN → 0 */
}

/** 行深度定点值的偏置：步进误差让下端略低于真值时仍是正数（误差最多几百个单位） */
#define ZFIX_OFF 0x10000u

/**
 * 一行的定点深度（17.15，加 ZFIX_OFF 偏置的 uint32）：两端在平面上精确求值并夹到 [0, 65535]，
 * 中间线性步进。uint32 运算回绕有定义：循环在末像素之后多加的一步即使越界也无害。
 * 逐像素 zi = max(Z - OFF, 0) >> 15（截断，同旧的 (uint32_t)z）；上端离 65536 还有 32768 单位余量。
 */
GP_HOT static inline void row_zfix(const gp_tri_t *t, int L, int R, float fy, uint32_t *Z,
                                   uint32_t *dZ)
{
    const float zl = zclamp(plane_at(&t->z, (float)L, fy));
    const int32_t Z0 = (int32_t)(zl * 32768.0f + 0.5f);
    *Z = (uint32_t)Z0 + ZFIX_OFF;
    *dZ = 0;
    const int n = R - L;
    if (n > 0 && n <= GP_TILE) {
        const float zr = zclamp(plane_at(&t->z, (float)R, fy));
        const int32_t Z1 = (int32_t)(zr * 32768.0f + 0.5f);
        *dZ = (uint32_t)(int32_t)((float)(Z1 - Z0) * k_inv_row[n]);
    }
}

static inline uint32_t zfix16(uint32_t Z)
{
    return (Z > ZFIX_OFF ? Z - ZFIX_OFF : 0u) >> 15;
}

/* 1/n，n = 0..max(SEG, PX_SEG)（常量折叠，与运行时 1.0f / n 逐位相同） */
static const float k_inv[PX_SEG + 1] = {0.0f, 1.0f / 1, 1.0f / 2, 1.0f / 3, 1.0f / 4,
                                     1.0f / 5, 1.0f / 6, 1.0f / 7, 1.0f / 8, 1.0f / 9,
                                     1.0f / 10, 1.0f / 11, 1.0f / 12, 1.0f / 13, 1.0f / 14,
                                     1.0f / 15, 1.0f / 16};

/* ── 通用路径 ─────────────────────────────────────────────────────── */

GP_HOT static void raster_tri_generic(tile_ctx_t *tc, const gp_tri_t *t, const gp_rstate_t *s)
{
    const int n = t->n_att;
    const bool affine = (t->flags & GP_TF_AFFINE) != 0;
    const bool vm = (t->flags & GP_TF_VM) != 0 && s->vm && s->prog;

    const int o_col = t->o_col, o_t0 = t->o_t0, o_t1 = t->o_t1, o_fog = t->o_fog;
    const bool icol = o_col >= 0 && !(t->cflags & GP_TC_COLOR); /* 否则颜色是常量 const_rgba */

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
    float vmd[ESPGPU_VM_VARYINGS * 4];   /* 整段 VM 的每像素步进 */
    float vout[SEG][4];
    const int nv = (o_fog >= 0 ? n - 1 : n); /* VM：雾因子在最后，不是 varying */
    if (vm) {
        memset(vmv, 0, sizeof(vmv)); /* 未用到的 varying 恒为 0，只需清一次 */
        memset(vmd, 0, sizeof(vmd));
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
        uint32_t Z, dZ;
        row_zfix(t, L, R, fy, &Z, &dZ);
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

            /* VM：深度测试通过的像素过半（或不测深度）时整段一次跑（gp_vm_run_span，与逐像素
             * gp_vm_run 逐位相同的 varying 累加），否则逐像素跑——VM 很贵，不能给被深度挡住的像素算 */
            uint32_t vkeep = 0;
            bool vspan = false;
            if (vm) {
                int pass = len;
                if (dtest) {
                    pass = 0;
                    uint32_t Zt = Z;
                    for (int i = 0; i < len; i++, Zt += dZ) {
                        pass += (int)gp_cmp_m(zmask, zfix16(Zt), drow[xs + i]);
                    }
                }
                if (pass * 2 >= len && pass > 1) {
                    memcpy(vmv, va, sizeof(float) * (size_t)nv);
                    memcpy(vmd, vstep, sizeof(float) * (size_t)nv);
                    vmv[24] = fx + 0.5f;
                    vmd[24] = 1.0f;
                    vmv[25] = fy + 0.5f;
                    vmd[25] = 0.0f;
                    vmv[26] = (float)(Z > ZFIX_OFF ? Z - ZFIX_OFF : 0u) * (1.0f / (32768.0f * 65535.0f));
                    vmd[26] = (float)(int32_t)dZ * (1.0f / (32768.0f * 65535.0f));
                    vmv[27] = affine ? t->iw.c : plane_at(&t->iw, fx, fy);
                    vmd[27] = affine ? 0.0f : t->iw.dx;
                    vkeep = gp_vm_run_span(s->prog, s->consts, s->smp, vmv, vmd, len, tc->w->vm_regs, vout);
                    vspan = true;
                }
            }

            for (int i = 0; i < len; i++, Z += dZ) {
                const int x = xs + i;
                uint32_t zi = 0;
                if (dtest || dwrite) {
                    zi = zfix16(Z);
                }
                if (dtest && !gp_cmp_m(zmask, zi, drow[x])) {
                    goto next;
                }

                uint32_t out;
                if (vm && vspan) {
                    if (!((vkeep >> i) & 1u)) {
                        goto next;
                    }
                    out = float_rgba(vout[i]);
                } else if (vm) {
                    memcpy(vmv, va, sizeof(float) * (size_t)nv);
                    vmv[24] = (float)x + 0.5f;
                    vmv[25] = fy + 0.5f;
                    vmv[26] = (float)(Z > ZFIX_OFF ? Z - ZFIX_OFF : 0u) * (1.0f / (32768.0f * 65535.0f));
                    vmv[27] = affine ? t->iw.c : plane_at(&t->iw, (float)x, fy);
                    float o[4];
                    if (!gp_vm_run(s->prog, s->consts, s->smp, vmv, tc->w->vm_regs, o)) {
                        goto next;
                    }
                    out = float_rgba(o);
                } else {
                    const uint32_t C = icol ? float_rgba(&va[o_col]) : Cconst;
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

typedef struct {
    const uint16_t *tpx;
    const uint8_t *tix;
    uint16_t *cp, *dp;
    uint32_t mask_u, mask_v, shift_w, mr, mg, mb;
    const cmpf_t *zf;
    bool dtest, dwrite, modulate;
} fast_row_t;

GP_INLINE uint32_t fast_row_p2(const fast_row_t *f, const int idx, int len, uint32_t Z,
                               uint32_t dZ, int32_t U, int32_t dU, int32_t V, int32_t dV)
{
    uint32_t frags = 0;
    for (int i = 0; i < len; i++, Z += dZ, U += dU, V += dV) {
        uint32_t zi = 0;
        if (f->dtest || f->dwrite) {
            zi = zfix16(Z);
            if (f->dtest && !cmpf(f->zf, zi, f->dp[i])) {
                continue;
            }
        }
        const uint32_t iu = (uint32_t)(U >> 16) & f->mask_u;
        const uint32_t iv = (uint32_t)(V >> 16) & f->mask_v;
        const uint32_t ti = (iv << f->shift_w) | iu;
        uint16_t px = idx ? f->tpx[f->tix[ti]] : f->tpx[ti];
        if (f->modulate) {
            const uint32_t r = (((uint32_t)(px >> 11) & 0x1F) * f->mr) >> 8;
            const uint32_t g = (((uint32_t)(px >> 5) & 0x3F) * f->mg) >> 8;
            const uint32_t b = (((uint32_t)px & 0x1F) * f->mb) >> 8;
            px = (uint16_t)((r << 11) | (g << 5) | b);
        }
        f->cp[i] = px;
        if (f->dwrite) {
            f->dp[i] = (uint16_t)zi;
        }
        frags++;
    }
    return frags;
}

/** idx：INDEX8 + 不透明调色板（GL 前端把 ≤256 色的纹理自动调色板化，Doom 的墙 / 地面都是这种） */
GP_HOT static void raster_tri_fast(tile_ctx_t *tc, const gp_tri_t *t, const gp_rstate_t *s)
{
    const bool idx = s->smp[0].tex->px == NULL;
    const gp_sampler_t *sm = &s->smp[0];
    const gp_tex_t *tex = sm->tex;
    const uint16_t *tpx = idx ? tex->pal : tex->px;
    const uint8_t *tix = tex->idx;
    (void)tix;
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
    cmpf_t zf;
    cmpf_init(&zf, s->depth_func);

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
        uint32_t Z, dZ;
        row_zfix(t, L, R, fy, &Z, &dZ);
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
                    /* REPEAT + 2 的幂：取模就是与掩码，无分支。最热的循环按存储方式各展开一份 */
                    const fast_row_t fr = {tpx, tix, crow + xs, drow ? drow + xs : NULL, mask_u, mask_v,
                                           (uint32_t)shift_w, mr, mg, mb, &zf, dtest, dwrite, modulate};
                    frags += idx ? fast_row_p2(&fr, 1, len, Z, dZ, U, dU, V, dV)
                                 : fast_row_p2(&fr, 0, len, Z, dZ, U, dU, V, dV);
                    Z += dZ * (uint32_t)len;
                } else {
                    for (int i = 0; i < len; i++, Z += dZ, U += dU, V += dV) {
                        const int x = xs + i;
                        uint32_t zi = 0;
                        if (dtest || dwrite) {
                            zi = zfix16(Z);
                            if (dtest && !cmpf(&zf, zi, drow[x])) {
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
                        const size_t ti = (size_t)iv * tw + iu;
                        uint16_t px = idx ? tpx[tix[ti]] : tpx[ti];
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
            for (int i = 0; i < len; i++, Z += dZ, u += du, v += dv) {
                const int x = xs + i;
                uint32_t zi = 0;
                if (dtest || dwrite) {
                    zi = zfix16(Z);
                    if (dtest && !cmpf(&zf, zi, drow[x])) {
                        continue;
                    }
                }
                const int iu = gp_wrap(gp_floor_i(u), tw, sm->wrap_u);
                const int iv = gp_wrap(gp_floor_i(v), th, sm->wrap_v);
                const size_t ti = (size_t)iv * tw + iu;
                uint16_t px = idx ? tpx[tix[ti]] : tpx[ti];
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

/* ── 像素管线快速路径 ─────────────────────────────────────────────── */
/*
 * 覆盖 3D 里最常见的组合：单纹理（最近邻 / 双线性；RGB565、RGB565+alpha、INDEX8 调色板、
 * R8 / 无调色板灰度）× { TEX, TEX_MOD_COLOR, FLAT }、颜色插值或常量、雾、深度测试 / 写、
 * alpha 测试、常见混合（标准 alpha、加法、alpha 加法、乘法）。
 *
 * 每段（8 像素）仍做一次精确透视除法；段内所有量都是定点整数步进：
 *   纹理坐标 16.16、深度 17.15（row_zfix）、颜色 / 雾 8.8（+0.5 偏置，>> 8 即四舍五入），
 *   颜色两两打包在一个字里步进（r<<16|b、g<<16|a），段内每像素 6 次整数加法。
 * 颜色 / 雾在段端点夹到 [0,1] 后线性插值：建立时已确认三个顶点都在 [0,1]（GP_TF_UNIT），
 * 段端点是三角形内的像素中心，凸组合仍在 [0,1]，端点夹取与逐像素夹取等价。
 * 调制 / 雾 / 混合的舍入与通用路径逐位相同（gp_mul8，分别舍入再求和），雾 / 混合把两个 8 位通道
 * 打包（r<<16|b）一次乘法算两个通道；RGB565 展开用乘法（x·33 >> 2 == x<<3 | x>>2）。
 * 剩下的差异来自插值（定点步进 vs 浮点累加），以 RGB565 输出计不超过 1；例外：最近邻在 texel
 * 边界上偶尔取到相邻 texel、alpha 恰在阈值上时 alpha 测试翻转、±1 经过 565 帧缓冲量化后
 * 被叠加混合放大。（试过 a·(c+1) >> 8 与 0..256 权重的更便宜形式：每像素省约 10 条指令，
 * 但 ±1 以外的像素从 0.06% 增加到 0.3%，没有采用。）
 * 深度 / alpha 比较预先编码成一次无符号比较（cmpf_t），不查表、不 switch。
 *
 * 一段（≤ 8 像素）分几遍做（深度 → 取样 → 调制 → 雾 → alpha 测试 → 写出，见 px_seg）：
 * 每遍是活跃变量很少的小循环，开关（纹理存储方式、双线性、雾、alpha 测试、混合…）
 * 每段判断一次而不是逐像素；整个路径只有一份代码（IRAM 有限）。
 */

enum { PK_NONE = 0, PK_565, PK_IDX };
enum { PB_NONE = 0, PB_ALPHA, PB_ADD, PB_ADDA, PB_MUL };

#define PX_FF16 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
#define PX_FF64 PX_FF16, PX_FF16, PX_FF16, PX_FF16
/** 不透明 alpha 表：PK_IDX 没有 alpha 时按下标查它，省掉一个分支 */
static const uint8_t k_opaque[256] = {PX_FF64, PX_FF64, PX_FF64, PX_FF64};

typedef struct {
    /* 纹理 */
    const uint16_t *px;      /* PK_565：像素；PK_IDX：调色板（NULL = 亮度） */
    const uint8_t *idx;      /* PK_IDX：下标 */
    const uint8_t *a8;       /* PK_565：逐 texel alpha（NULL = 不透明）；PK_IDX：按下标的 alpha 表 */
    const gp_sampler_t *smp; /* 超大坐标的慢速段用通用采样 */
    int tw, th;
    uint32_t mu, mv;         /* REPEAT 且 2 的幂：n-1；否则全 1，随后做范围检查 */
    uint8_t wu, wv;
    uint8_t rep_u, rep_v;    /* 段起点折回（REPEAT / MIRROR） */
    uint8_t lin;             /* 双线性 */
    float fpu, fpv;          /* 折回周期 */
    float ifpu, ifpv;        /* 1 / 周期（折回用乘法，不调 floorf、不做除法） */
    float wlo_u, whi_u, wlo_v, whi_v; /* 段起点在 [lo, hi) 内就不用折回：2 的幂 REPEAT 靠掩码，范围放宽到 ±4096 */
    /* 片元 */
    uint8_t mod;             /* 输出 = T × C（否则 = T） */
    uint8_t fog;
    uint8_t ztest, zwrite, zany;
    uint8_t atest, blend, need_a;
    cmpf_t zf, af;
    uint32_t aref;
    uint32_t fog_rb, fog_g;  /* 雾色 r<<16|b、g */
    uint32_t crb, cga;       /* 常量色（无颜色属性或 GP_TC_COLOR），8.8 打包（同 RB / GA 步进量） */
    uint8_t ccol;            /* 颜色是常量：调制遍直接乘常数，不逐像素算插值 */
    uint8_t cgray;           /* 常量色 r == g == b（Doom 的亮度）：r、b 打包一次乘 */
    uint8_t cr, cg, cb, ca;
    const uint32_t *fog_rbt; /* 雾表（worker 里）：f → mul_rb(雾色 rb, 255-f) */
    const uint8_t *fog_gt;   /*                  f → gp_mul8(雾色 g, 255-f) */
} pxs_t;

/** 打包通道 × 标量 /255（每半字与 gp_mul8 逐位相同；每半字 ≤ 65153，两半互不进位） */
static inline uint32_t mul_rb(uint32_t rb, uint32_t f)
{
    const uint32_t t = rb * f + 0x00800080u;
    return ((t + ((t >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
}

/**
 * gp_mul8(a, f) + gp_mul8(b, 255 - f)：与通用路径的雾 / 标准 alpha 混合逐位相同（分别舍入再求和）。
 * 和不超过 255（gp_mul8 单调且 gp_mul8(255, f) == f），不用夹取，打包的两半也不会进位。
 */
static inline uint32_t mix_rb(uint32_t a_rb, uint32_t b_rb, uint32_t f)
{
    return mul_rb(a_rb, f) + mul_rb(b_rb, 255u - f);
}

static inline uint32_t mix_1(uint32_t a, uint32_t b, uint32_t f)
{
    return gp_mul8(a, f) + gp_mul8(b, 255u - f);
}

/** RGB565 的 r、b 展开到 8 位并打包：x·33 >> 2 == (x<<3)|(x>>2)，两个通道一次乘法 */
static inline uint32_t rb_of565(uint32_t p)
{
    const uint32_t x = ((p & 0xF800u) << 5) | (p & 0x1Fu);
    return ((x * 33u) >> 2) & 0x00FF00FFu;
}

static inline uint32_t g_of565(uint32_t p)
{
    return (((p >> 5) & 63u) * 65u) >> 4; /* == (g<<2)|(g>>4) */
}

static inline uint32_t pack_rbg(uint32_t rb, uint32_t g)
{
    return ((rb >> 8) & 0xF800u) | ((g & 0xFCu) << 3) | ((rb & 0xFFu) >> 3);
}

/** 取一个 texel → rb = r<<16|b，ga = a<<16|g */
GP_INLINE void px_texel(const pxs_t *k, const int kind, uint32_t i, uint32_t *rb, uint32_t *ga)
{
    if (kind == PK_565) {
        const uint32_t p = k->px[i];
        *rb = rb_of565(p);
        *ga = (k->a8 ? (uint32_t)k->a8[i] << 16 : 0x00FF0000u) | g_of565(p);
    } else {
        const uint32_t v = k->idx[i];
        if (k->px) {
            const uint32_t p = k->px[v];
            *rb = rb_of565(p);
            *ga = ((uint32_t)k->a8[v] << 16) | g_of565(p);
        } else {
            *rb = (v << 16) | v;
            *ga = ((uint32_t)k->a8[v] << 16) | v;
        }
    }
}

/** 不常走的折回（CLAMP / MIRROR / 非 2 的幂 REPEAT 越界）：不内联，内核保持紧凑 */
GP_HOT GP_NOINLINE static int px_wrap(int v, int n, uint8_t mode)
{
    return gp_wrap(v, n, mode);
}

/**
 * 双线性取样（U、V 已减去半个 texel）：先横后纵两次 8 位权重插值，打包的两个通道一起算
 * （每半字 ≤ 65408，不进位）。单独成函数：最近邻内核的循环少一大堆活跃变量（寄存器溢出）。
 */
GP_INLINE void px_bilin(const pxs_t *k, const int kind, int32_t U, int32_t V, uint32_t *trb,
                        uint32_t *tga)
{
    const int tw = k->tw, th = k->th;
    const uint32_t ax = ((uint32_t)U >> 8) & 255u;
    const uint32_t ay = ((uint32_t)V >> 8) & 255u;
    int xa = (int)((uint32_t)(U >> 16) & k->mu);
    int xb = (int)((uint32_t)((U >> 16) + 1) & k->mu);
    int ya = (int)((uint32_t)(V >> 16) & k->mv);
    int yb = (int)((uint32_t)((V >> 16) + 1) & k->mv);
    if ((unsigned)xa >= (unsigned)tw) xa = px_wrap(xa, tw, k->wu);
    if ((unsigned)xb >= (unsigned)tw) xb = px_wrap(xb, tw, k->wu);
    if ((unsigned)ya >= (unsigned)th) ya = px_wrap(ya, th, k->wv);
    if ((unsigned)yb >= (unsigned)th) yb = px_wrap(yb, th, k->wv);
    const uint32_t ra = (uint32_t)(ya * tw), rbw = (uint32_t)(yb * tw);
    uint32_t c00, g00, c10, g10, c01, g01, c11, g11;
    px_texel(k, kind, ra + (uint32_t)xa, &c00, &g00);
    px_texel(k, kind, ra + (uint32_t)xb, &c10, &g10);
    px_texel(k, kind, rbw + (uint32_t)xa, &c01, &g01);
    px_texel(k, kind, rbw + (uint32_t)xb, &c11, &g11);
    const uint32_t nx = 256u - ax, ny = 256u - ay;
    const uint32_t t0 = ((c00 * nx + c10 * ax + 0x00800080u) >> 8) & 0x00FF00FFu;
    const uint32_t t1 = ((c01 * nx + c11 * ax + 0x00800080u) >> 8) & 0x00FF00FFu;
    *trb = ((t0 * ny + t1 * ay + 0x00800080u) >> 8) & 0x00FF00FFu;
    const uint32_t s0 = ((g00 * nx + g10 * ax + 0x00800080u) >> 8) & 0x00FF00FFu;
    const uint32_t s1 = ((g01 * nx + g11 * ax + 0x00800080u) >> 8) & 0x00FF00FFu;
    *tga = ((s0 * ny + s1 * ay + 0x00800080u) >> 8) & 0x00FF00FFu;
}

GP_HOT GP_NOINLINE static void px_bilin_565(const pxs_t *k, int32_t U, int32_t V, uint32_t *trb,
                                            uint32_t *tga)
{
    px_bilin(k, PK_565, U, V, trb, tga);
}

GP_HOT GP_NOINLINE static void px_bilin_idx(const pxs_t *k, int32_t U, int32_t V, uint32_t *trb,
                                            uint32_t *tga)
{
    px_bilin(k, PK_IDX, U, V, trb, tga);
}

/** 混合（不内联：只有半透明 / 精灵走到）→ 写回的 RGB565 */
GP_HOT GP_NOINLINE static uint32_t px_blend(const pxs_t *k, uint32_t rb, uint32_t g, uint32_t a,
                                            uint32_t d)
{
    const uint32_t drb = rb_of565(d), dg = g_of565(d);
    switch (k->blend) {
    case PB_ALPHA:
        rb = mix_rb(rb, drb, a);
        g = mix_1(g, dg, a);
        break;
    case PB_ADD:
    case PB_ADDA: {
        if (k->blend == PB_ADDA) {
            rb = mul_rb(rb, a);
            g = gp_mul8(g, a);
        }
        const uint32_t sum = rb + drb;                   /* 每半字 ≤ 510 */
        const uint32_t ov = (sum >> 8) & 0x00010001u;    /* 溢出位 */
        rb = (sum | (ov * 255u)) & 0x00FF00FFu;          /* 饱和到 255 */
        g += dg;
        g = g > 255u ? 255u : g;
        break;
    }
    default: /* PB_MUL */
        rb = (gp_mul8(rb >> 16, drb >> 16) << 16) | gp_mul8(rb & 255u, drb & 255u);
        g = gp_mul8(g, dg);
        break;
    }
    return pack_rbg(rb, g);
}

/**
 * 片元的着色 / 雾 / alpha 测试 / 混合 / 写出。返回 1 = 写了这个片元。
 * RB = r<<16|b、GA = g<<16|a、F 都是 8.8（含 +0.5）。
 */
GP_INLINE uint32_t px_out(const pxs_t *k, uint32_t trb, uint32_t tga, uint32_t RB, uint32_t GA,
                          uint32_t F, uint16_t *cp, uint16_t *dp, uint32_t zi)
{
    uint32_t rb, g, a;
    if (k->mod) {
        const uint32_t r = gp_mul8(trb >> 16, RB >> 24);
        const uint32_t b = gp_mul8(trb & 255u, (RB >> 8) & 255u);
        g = gp_mul8(tga & 255u, GA >> 24);
        a = k->need_a ? gp_mul8(tga >> 16, (GA >> 8) & 255u) : 255u;
        rb = (r << 16) | b;
    } else {
        rb = trb;
        g = tga & 255u;
        a = tga >> 16;
    }
    if (k->fog) {
        const uint32_t f = F >> 8;
        rb = mix_rb(rb, k->fog_rb, f);
        g = mix_1(g, k->fog_g, f);
    }
    if (k->atest && !cmpf(&k->af, a, k->aref)) {
        return 0;
    }
    *cp = (uint16_t)(k->blend ? px_blend(k, rb, g, a, *cp) : pack_rbg(rb, g));
    if (k->zwrite) {
        *dp = (uint16_t)zi;
    }
    return 1;
}

/** 一段的定点状态 */
typedef struct {
    uint32_t Z, dZ;
    uint32_t RB, dRB, GA, dGA, F, dF;
} px_seg_t;

/**
 * 慢速段：纹理坐标超出定点范围（CLAMP 下坐标远超纹理、极端缩小）时逐像素 float 采样。
 * 病态值（|u| 上亿 / NaN）钉在 0，免得 float→int 越界。
 */
GP_HOT GP_NOINLINE static uint32_t px_seg_slow(const pxs_t *k, const px_seg_t *sg, uint16_t *crow,
                                               uint16_t *drow, int xs, int len, float u, float du,
                                               float v, float dv)
{
    if (!(fabsf(u) < 1e8f) || !(fabsf(du) < 1e7f)) {
        u = 0.0f;
        du = 0.0f;
    }
    if (!(fabsf(v) < 1e8f) || !(fabsf(dv) < 1e7f)) {
        v = 0.0f;
        dv = 0.0f;
    }
    uint32_t frags = 0;
    uint32_t Z = sg->Z;
    uint32_t RB = sg->RB, GA = sg->GA, F = sg->F;
    for (int i = 0; i < len; i++, u += du, v += dv, Z += sg->dZ, RB += sg->dRB, GA += sg->dGA,
             F += sg->dF) {
        const int x = xs + i;
        uint32_t zi = 0;
        if (k->zany) {
            zi = zfix16(Z);
            if (k->ztest && !cmpf(&k->zf, zi, drow[x])) {
                continue;
            }
        }
        const uint32_t c = k->lin ? gp_sample_linear(k->smp, u, v) : gp_sample_nearest(k->smp, u, v);
        frags += px_out(k, c & 0x00FF00FFu, (c >> 8) & 0x00FF00FFu, RB, GA, F, &crow[x],
                        drow ? &drow[x] : NULL, zi);
    }
    return frags;
}

/** [0,1] → 8.8 定点（+0.5）：v·255·256 + 128 ∈ [128, 65408] */
static inline int32_t fix88(float v)
{
    return (int32_t)(unit_f(v) * (255.0f * 256.0f) + 128.0f);
}


/** 两个 8.8 量打包成 hi<<16|lo 的起点与步进（lo 的步进为负时借位与 hi 的进位相抵） */
static inline void pack88(int32_t hs, int32_t he, int32_t ls, int32_t le, float il, uint32_t *v,
                          uint32_t *dv)
{
    const int32_t dh = (int32_t)((float)(he - hs) * il);
    const int32_t dl = (int32_t)((float)(le - ls) * il);
    *v = ((uint32_t)hs << 16) + (uint32_t)ls;
    *dv = ((uint32_t)dh << 16) + (uint32_t)dl;
}

/** 最近邻取样遍：kind 为常量，两种存储方式各展开一份（都在 px_seg 里） */
GP_INLINE void px_fetch_n(const pxs_t *k, const int kind, const uint8_t *ix, int n, int32_t U,
                          int32_t dU, int32_t V, int32_t dV, uint32_t *trb, uint32_t *tga)
{
    const uint32_t mu = k->mu, mv = k->mv;
    const int tw = k->tw, th = k->th;
    for (int j = 0; j < n; j++) {
        const int32_t i = ix[j];
        int iu = (int)((uint32_t)((U + i * dU) >> 16) & mu);
        int iv = (int)((uint32_t)((V + i * dV) >> 16) & mv);
        if ((unsigned)iu >= (unsigned)tw) iu = px_wrap(iu, tw, k->wu);
        if ((unsigned)iv >= (unsigned)th) iv = px_wrap(iv, th, k->wv);
        px_texel(k, kind, (uint32_t)(iv * tw + iu), &trb[j], &tga[j]);
    }
}

/**
 * 一段（≤ PX_SEG，16 像素）的深度遍：通过的像素下标无分支地压紧到 ix[]，深度值存 zz[]。
 * 在段的属性建立（透视除法等）之前做：整段被挡住时那些都省掉（Doom 按纹理排序画，
 * 被深度挡住的段约占两成）。返回通过的像素数。
 */
GP_INLINE int px_depth(const pxs_t *k, uint32_t Z, uint32_t dZ, const uint16_t *dp, int len,
                       uint8_t *ix, uint16_t *zz)
{
    int n = 0;
    if (k->ztest) {
        const cmpf_t zf = k->zf;
        for (int i = 0; i < len; i++, Z += dZ) {
            const uint32_t zi = zfix16(Z);
            ix[n] = (uint8_t)i;
            zz[n] = (uint16_t)zi;
            n += cmpf(&zf, zi, dp[i]) ? 1 : 0;
        }
    } else {
        for (int i = 0; i < len; i++, Z += dZ) {
            ix[i] = (uint8_t)i;
            zz[i] = (uint16_t)zfix16(Z);
        }
        n = len;
    }
    return n;
}

/**
 * 融合段：最近邻 + 常量色调制 + 可选雾 + 无 alpha 测试 / 混合。
 * 实测（Doom / PrBoom gl，320x200）：px 段 100% 是常量色（glColor 每个三角形一次）、
 * 一半 r/g/b 不同，所以热路径是「取样 → 常量色调制 → 雾 → 写出」四遍。
 * 合成一遍：中间值留在寄存器里，省掉 trb / tga 在栈上的来回存取和每遍重算 ix[j]。
 * 运算顺序与多遍版本完全相同，输出逐位一致。
 */
/**
 * 一个片元：取样 → 常量色调制 → 雾 → 输出 RGB565。
 * 单独成 inline 函数是为了让 px_fused_n 手动两像素展开时两次调用互相独立：
 * Xtensa 是顺序流水，一个像素的链（取样依赖 → 乘 → 乘）不展开就只能干等。
 */
GP_INLINE uint32_t px_fused_px(const pxs_t *k, int kind, int i, const px_seg_t *sg, int32_t U,
                               int32_t dU, int32_t V, int32_t dV)
{
    const uint32_t mu = k->mu, mv = k->mv;
    const int tw = k->tw, th = k->th;
    int iu = (int)((uint32_t)((U + (uint32_t)i * dU) >> 16) & mu);
    int iv = (int)((uint32_t)((V + (uint32_t)i * dV) >> 16) & mv);
    if ((unsigned)iu >= (unsigned)tw) iu = px_wrap(iu, tw, k->wu);
    if ((unsigned)iv >= (unsigned)th) iv = px_wrap(iv, th, k->wv);
    const uint32_t ti = (uint32_t)(iv * tw + iu);
    uint32_t rb, g;
    if (kind == PK_565) {
        const uint32_t p = k->px[ti];
        rb = rb_of565(p);
        g = g_of565(p);
    } else {
        const uint32_t v = k->idx[ti];
        if (k->px) {
            const uint32_t p = k->px[v]; /* 调色板 */
            rb = rb_of565(p);
            g = g_of565(p);
        } else {
            rb = (v << 16) | v;
            g = v;
        }
    }
    if (k->cgray) {
        rb = mul_rb(rb, k->cr);
        g = gp_mul8(g, k->cr);
    } else {
        rb = (gp_mul8(rb >> 16, k->cr) << 16) | gp_mul8(rb & 255u, k->cb);
        g = gp_mul8(g, k->cg);
    }
    if (k->fog) {
        const uint32_t f = ((sg->F + (uint32_t)i * sg->dF) >> 8) & 255u;
        rb = mul_rb(rb, f) + k->fog_rbt[f];
        g = gp_mul8(g, f) + k->fog_gt[f];
    }
    return pack_rbg(rb, g);
}

GP_INLINE uint32_t px_fused_n(const pxs_t *k, int kind, const px_seg_t *sg, uint16_t *cp,
                              uint16_t *dp, int n, const uint8_t *ix, const uint16_t *zz,
                              int32_t U, int32_t dU, int32_t V, int32_t dV, bool dense)
{
    const bool zw = k->zwrite != 0;
    int j = 0;
    /* 两像素展开：两次取样 / 调制 / 雾 / 打包互不依赖，流水线能交错起来 */
    if (dense) {
        for (; j + 1 < n; j += 2) {
            const uint32_t c0 = px_fused_px(k, kind, j, sg, U, dU, V, dV);
            const uint32_t c1 = px_fused_px(k, kind, j + 1, sg, U, dU, V, dV);
            cp[j] = (uint16_t)c0;
            cp[j + 1] = (uint16_t)c1;
            if (zw) {
                dp[j] = zz[j];
                dp[j + 1] = zz[j + 1];
            }
        }
    }
    for (; j < n; j++) {
        /* 整段的像素都通过深度时没有压缩，下标直接是 j（省掉 ix 的间接访存） */
        const int i = dense ? j : ix[j];
        cp[i] = (uint16_t)px_fused_px(k, kind, i, sg, U, dU, V, dV);
        if (zw) {
            dp[i] = zz[j];
        }
    }
    return (uint32_t)n;
}

/**
 * 一段（≤ PX_SEG，16 像素）深度之后的几遍：取样 → 调制 → 雾 → alpha 测试 → 写出。
 * 每遍是只有几个活跃变量的小循环（不溢出寄存器），开关在段级判断一次而不是逐像素；
 * 只算 px_depth 留下的 n 个像素（逐像素的定点量由 起点 + 下标 × 步进 一次乘法得到）。
 */
GP_INLINE uint32_t px_seg(const pxs_t *k, int kind, bool lin, const px_seg_t *sg, uint16_t *cp,
                          uint16_t *dp, int n, uint8_t *ix, uint16_t *zz, int32_t U, int32_t dU,
                          int32_t V, int32_t dV, int len)
{
    uint32_t trb[PX_SEG], tga[PX_SEG];

    /* 融合路径：最近邻 + 常量色调制（Doom 的墙 / 地板，实测 100% 常量色）→ 单遍 */
    if (!lin && kind != PK_NONE && k->mod && k->ccol && !k->atest && !k->blend) {
        return px_fused_n(k, kind, sg, cp, dp, n, ix, zz, U, dU, V, dV, n == len);
    }

    /* 取样 → trb = r<<16|b，tga = a<<16|g */
    if (kind == PK_NONE) {
        for (int j = 0; j < n; j++) {
            trb[j] = 0x00FF00FFu;
            tga[j] = 0x00FF00FFu;
        }
    } else if (!lin) {
        if (kind == PK_565) {
            px_fetch_n(k, PK_565, ix, n, U, dU, V, dV, trb, tga);
        } else {
            px_fetch_n(k, PK_IDX, ix, n, U, dU, V, dV, trb, tga);
        }
    } else {
        for (int j = 0; j < n; j++) {
            const int32_t i = ix[j];
            if (kind == PK_565) {
                px_bilin_565(k, U + i * dU, V + i * dV, &trb[j], &tga[j]);
            } else {
                px_bilin_idx(k, U + i * dU, V + i * dV, &trb[j], &tga[j]);
            }
        }
    }

    /* 调制 T × C（gp_mul8，与通用路径相同；mul_rb 每半字与 gp_mul8 逐位相同） */
    if (k->mod && k->ccol) {
        const uint32_t cr = k->cr, cg = k->cg, cb = k->cb, ca = k->ca;
        const bool na = k->need_a != 0;
        if (k->cgray) {
            for (int j = 0; j < n; j++) {
                const uint32_t q = tga[j];
                const uint32_t a = na ? gp_mul8(q >> 16, ca) : 255u;
                trb[j] = mul_rb(trb[j], cr);
                tga[j] = (a << 16) | gp_mul8(q & 255u, cr);
            }
        } else {
            for (int j = 0; j < n; j++) {
                const uint32_t t = trb[j], q = tga[j];
                const uint32_t a = na ? gp_mul8(q >> 16, ca) : 255u;
                trb[j] = (gp_mul8(t >> 16, cr) << 16) | gp_mul8(t & 255u, cb);
                tga[j] = (a << 16) | gp_mul8(q & 255u, cg);
            }
        }
    } else if (k->mod) {
        const uint32_t RB = sg->RB, GA = sg->GA, dRB = sg->dRB, dGA = sg->dGA;
        const bool na = k->need_a != 0;
        for (int j = 0; j < n; j++) {
            const uint32_t i = ix[j];
            const uint32_t rbc = RB + i * dRB, gac = GA + i * dGA;
            const uint32_t t = trb[j], q = tga[j];
            const uint32_t r = gp_mul8(t >> 16, rbc >> 24);
            const uint32_t b = gp_mul8(t & 255u, (rbc >> 8) & 255u);
            const uint32_t g = gp_mul8(q & 255u, gac >> 24);
            const uint32_t a = na ? gp_mul8(q >> 16, (gac >> 8) & 255u) : 255u;
            trb[j] = (r << 16) | b;
            tga[j] = (a << 16) | g;
        }
    }

    /* 雾 */
    if (k->fog) {
        /* mix_rb / mix_1 的雾色那一半只和 f 有关，查表（逐位相同）。f 由夹过的端点插值而来，
         * 总在 [0,255]；& 255 只是防御 */
        const uint32_t F = sg->F, dF = sg->dF;
        const uint32_t *frt = k->fog_rbt;
        const uint8_t *fgt = k->fog_gt;
        for (int j = 0; j < n; j++) {
            const uint32_t f = ((F + ix[j] * dF) >> 8) & 255u;
            const uint32_t q = tga[j];
            trb[j] = mul_rb(trb[j], f) + frt[f];
            tga[j] = (q & 0x00FF0000u) | (gp_mul8(q & 255u, f) + fgt[f]);
        }
    }

    /* alpha 测试：再压紧一次 */
    if (k->atest) {
        const cmpf_t af = k->af;
        const uint32_t aref = k->aref;
        int m = 0;
        for (int j = 0; j < n; j++) {
            ix[m] = ix[j];
            zz[m] = zz[j];
            trb[m] = trb[j];
            tga[m] = tga[j];
            m += cmpf(&af, tga[j] >> 16, aref) ? 1 : 0;
        }
        n = m;
    }

    /* 写出 */
    if (k->blend) {
        for (int j = 0; j < n; j++) {
            uint16_t *c = cp + ix[j];
            *c = (uint16_t)px_blend(k, trb[j], tga[j] & 255u, tga[j] >> 16, *c);
        }
    } else {
        for (int j = 0; j < n; j++) {
            cp[ix[j]] = (uint16_t)pack_rbg(trb[j], tga[j]);
        }
    }
    if (k->zwrite) {
        for (int j = 0; j < n; j++) {
            dp[ix[j]] = zz[j];
        }
    }
    return (uint32_t)n;
}

/** 像素管线快速路径的三角形 × tile（一份代码；纹理存储方式在每段的取样遍里分派） */
GP_HOT static void raster_tri_px(tile_ctx_t *tc, const gp_tri_t *t, const pxs_t *k, int kind)
{
    const int y0 = t->by0 > tc->ty0 ? t->by0 : tc->ty0;
    const int y1 = t->by1 < tc->ty1 ? t->by1 : tc->ty1;
    const int lo = t->bx0 > tc->tx0 ? t->bx0 : tc->tx0;
    const int hi = t->bx1 < tc->tx1 ? t->bx1 : tc->tx1;
    span_t sp;
    if (!span_init(&sp, t, lo, hi, y0, y1)) {
        return;
    }
    gp_worker_t *w = tc->w;
    const bool affine = (t->flags & GP_TF_AFFINE) != 0;
    const bool lin = kind != PK_NONE && k->lin;

    /* 需要插值的量：槽 0 u、1 v、2..5 r g b a、6 雾。平面先拷到栈上（三角形记录在 PSRAM 的 arena 里） */
    gp_plane_t P[7];
    int sl[7], na = 0;
    if (kind != PK_NONE) {
        P[na] = t->att[t->o_t0]; sl[na++] = 0;
        P[na] = t->att[t->o_t0 + 1]; sl[na++] = 1;
    }
    const bool icol = k->mod && !k->ccol;
    if (icol) {
        for (int ch = 0; ch < 4; ch++) {
            P[na] = t->att[t->o_col + ch]; sl[na++] = 2 + ch;
        }
    }
    const bool ifog = k->fog;
    if (ifog) {
        P[na] = t->att[t->o_fog]; sl[na++] = 6;
    }
    const gp_plane_t piw = t->iw;

    uint32_t frags = 0, rows = 0, segs = 0, tested = 0, cy_seg = 0, cy_pix = 0;

    px_seg_t sg;
    sg.RB = k->crb;
    sg.GA = k->cga;
    sg.dRB = sg.dGA = 0;
    sg.F = 0;
    sg.dF = 0;
    float sv[7] = {0}, ev[7] = {0};
    uint8_t ix[PX_SEG];
    uint16_t zz[PX_SEG];

    for (int y = y0; y <= y1; y++) {
        int L, R;
        if (!span_next(&sp, &L, &R)) {
            continue;
        }
        rows++;
        uint16_t *crow = tc->cbuf + (size_t)(y - tc->ty0) * tc->cstride - tc->tx0;
        uint16_t *drow = tc->dbuf ? tc->dbuf + (size_t)(y - tc->ty0) * GP_TILE - tc->tx0 : NULL;
        const float fy = (float)y;
        row_zfix(t, L, R, fy, &sg.Z, &sg.dZ);
        bool have_end = false;
        /* 一行内 1/w 几乎不变（不抬头低头时的地板 / 天花板、远处的墙）：w = 1/(1/w) 在行内
         * 线性插值，整行只除两次，不再每段除。1/w 在行内相对变化 ε 时，线性插值 w 的相对误差
         * 约 ε²/4，texel 坐标的误差约 ε²/4·|u|。要求 ε ≤ 2e-3 且 ε²·max(|u|,|v|) ≤ 6e-5，
         * 即误差不超过定点精度（1/65536 texel）；颜色 / 雾在 [0,1]，误差可以忽略。 */
        float wsl = 0.0f, kw = 0.0f;
        bool wrow = false;
        if (!affine && L < R) {
            const float iwl = plane_at(&piw, (float)L, fy), iwr = plane_at(&piw, (float)R, fy);
            const float d = fabsf(iwr - iwl), a = fabsf(iwl);
            if (iwl != 0.0f && iwr != 0.0f && d <= 2e-3f * a) {
                wsl = 1.0f / iwl;
                wrow = true;
                if (kind != PK_NONE) {
                    const float um = fabsf(plane_at(&P[0], (float)L, fy) * wsl) +
                                     fabsf(plane_at(&P[1], (float)L, fy) * wsl) + 1.0f;
                    wrow = d * d * um <= 6e-5f * a * a;
                }
                if (wrow) {
                    kw = (1.0f / iwr - wsl) * k_inv_row[R - L];
                }
            }
        }
        const float fl = (float)L;

        for (int xs = L; xs <= R; xs += PX_SEG) {
            const int len = (R - xs + 1) < PX_SEG ? (R - xs + 1) : PX_SEG;
            tested += (uint32_t)len;
            /* 深度先测：整段被挡住就不做下面的透视除法 / 定点化 */
            const int n = px_depth(k, sg.Z, sg.dZ, drow ? drow + xs : NULL, len, ix, zz);
            if (n == 0) {
                have_end = false; /* 下一段的起点要重新算 */
                goto seg_done;
            }
            {
                const uint32_t cy0 = gp_cycles();
                const int steps = (xs + PX_SEG <= R) ? PX_SEG : len - 1;
                const float fx = (float)xs;
                const float fe = fx + (float)steps;
                if (affine) {
                    for (int j = 0; j < na; j++) {
                        sv[sl[j]] = plane_at(&P[j], fx, fy);
                        ev[sl[j]] = plane_at(&P[j], fe, fy);
                    }
                } else {
                    if (have_end) {
                        for (int j = 0; j < na; j++) {
                            sv[sl[j]] = ev[sl[j]];
                        }
                    } else {
                        float ws = wsl + (fx - fl) * kw;
                        if (!wrow) {
                            const float iw_s = plane_at(&piw, fx, fy);
                            ws = iw_s != 0.0f ? 1.0f / iw_s : 0.0f;
                        }
                        for (int j = 0; j < na; j++) {
                            sv[sl[j]] = plane_at(&P[j], fx, fy) * ws;
                        }
                    }
                    /* 段终点：中间段取下一段起点（可复用，1/w 每段只除一次）；最后一段取 R 本身 */
                    if (steps > 0) {
                        float we = wsl + (fe - fl) * kw;
                        if (!wrow) {
                            const float iw_e = plane_at(&piw, fe, fy);
                            we = iw_e != 0.0f ? 1.0f / iw_e : 0.0f;
                        }
                        for (int j = 0; j < na; j++) {
                            ev[sl[j]] = plane_at(&P[j], fe, fy) * we;
                        }
                    } else {
                        for (int j = 0; j < na; j++) {
                            ev[sl[j]] = sv[sl[j]];
                        }
                    }
                    have_end = true;
                }
                const float il = k_inv[steps];

                if (icol) {
                    pack88(fix88(sv[2]), fix88(ev[2]), fix88(sv[4]), fix88(ev[4]), il, &sg.RB, &sg.dRB);
                    pack88(fix88(sv[3]), fix88(ev[3]), fix88(sv[5]), fix88(ev[5]), il, &sg.GA, &sg.dGA);
                }
                if (ifog) {
                    const int32_t fs = fix88(sv[6]), fe8 = fix88(ev[6]);
                    sg.F = (uint32_t)fs;
                    sg.dF = (uint32_t)(int32_t)((float)(fe8 - fs) * il);
                }

                int32_t U = 0, V = 0, dU = 0, dV = 0;
                if (kind != PK_NONE) {
                    float u = sv[0], v = sv[1];
                    const float du = (ev[0] - u) * il, dv = (ev[1] - v) * il;
                    /* 折回起点（周期不变，减的是周期的整数倍，结果精确）：REPEAT 到 [0, n)，MIRROR 到
                     * [0, 2n)。2 的幂 REPEAT 的 texel 下标本来就用掩码，只有坐标太大放不进定点时才折回。
                     * 乘倒数 + gp_floor_i 代替 floorf(u / n)：差一时起点略出 [0, n)，逐像素的越界
                     * 检查（px_wrap）会接住，取到的 texel 不变 */
                    if (k->rep_u && !(u >= k->wlo_u && u < k->whi_u)) {
                        u -= (float)gp_floor_i(u * k->ifpu) * k->fpu;
                    }
                    if (k->rep_v && !(v >= k->wlo_v && v < k->whi_v)) {
                        v -= (float)gp_floor_i(v * k->ifpv) * k->fpv;
                    }
                    const float uo = lin ? u - 0.5f : u, vo = lin ? v - 0.5f : v;
                    const float uz = uo + du * (float)(len - 1);
                    const float vz = vo + dv * (float)(len - 1);
                    if (!(fabsf(uo) < 16000.0f && fabsf(uz) < 16000.0f && fabsf(vo) < 16000.0f &&
                          fabsf(vz) < 16000.0f)) {
                        frags += px_seg_slow(k, &sg, crow, drow, xs, len, u, du, v, dv);
                        goto seg_done;
                    }
                    /* 四舍五入到 1/65536（与 fast 路径相同）；段两端都在 ±16000 内，int32 不会溢出 */
                    U = gp_floor_i(uo * 65536.0f + 0.5f);
                    V = gp_floor_i(vo * 65536.0f + 0.5f);
                    dU = len > 1 ? gp_floor_i(du * 65536.0f + 0.5f) : 0;
                    dV = len > 1 ? gp_floor_i(dv * 65536.0f + 0.5f) : 0;
                }
                const uint32_t cy1 = gp_cycles();
                frags += px_seg(k, kind, lin, &sg, crow + xs, drow ? drow + xs : NULL, n, ix, zz, U,
                                dU, V, dV, len);
                cy_pix += gp_cycles() - cy1;
                cy_seg += cy1 - cy0;
                segs++;
            }
        seg_done:
            sg.Z += sg.dZ * len;
        }
    }
    w->fragments += frags;
    w->n_px_rows += rows;
    w->n_px_segs += segs;
    w->n_px_tested += tested;
    w->cy_px_seg += cy_seg;
    w->cy_px_pix += cy_pix;
}

/** 能否走像素管线快速路径：能则填好 k、返回取样方式（PK_*），否则 -1 */
GP_HOT static int px_setup(const gp_tri_t *t, const gp_rstate_t *s, pxs_t *k, gp_worker_t *w)
{
#ifdef GP_TEST_NO_PX
    return -1; /* 测试用：强制走通用路径，与快速路径逐像素对比 */
#endif
    if (s->vm || (t->flags & GP_TF_VM) || !s->color_write) {
        return -1;
    }
    const uint8_t sh = s->shade;
    if (sh != ESPGPU_SHADE_FLAT && sh != ESPGPU_SHADE_TEX && sh != ESPGPU_SHADE_TEX_MOD_COLOR) {
        return -1;
    }
    if (t->o_col >= 0 && !(t->flags & GP_TF_UNIT)) {
        return -1; /* 顶点色越界：要逐像素夹取，走通用路径 */
    }
    memset(k, 0, sizeof(*k));
    int kind = PK_NONE;
    if (sh != ESPGPU_SHADE_FLAT) {
        const gp_sampler_t *sm = &s->smp[0];
        const gp_tex_t *tex = sm->tex;
        if (tex && t->o_t0 >= 0 && (tex->px || tex->idx)) {
            if (tex->px) {
                kind = PK_565;
                k->px = tex->px;
                k->a8 = tex->alpha;
            } else {
                /* INDEX8：调色板 + 调色板 alpha；R8：亮度、alpha 恒 1；
                 * 无调色板的 INDEX8：亮度 + 调色板 alpha（若有）——与 gp_texel 一致 */
                const bool r8 = tex->fmt == ESPGPU_FMT_R8;
                kind = PK_IDX;
                k->px = r8 ? NULL : tex->pal;
                k->idx = tex->idx;
                k->a8 = (!r8 && tex->pal_a) ? tex->pal_a : k_opaque;
            }
            k->lin = sm->filter == ESPGPU_FILTER_LINEAR;
            k->smp = sm;
            k->tw = tex->w;
            k->th = tex->h;
            k->wu = sm->wrap_u;
            k->wv = sm->wrap_v;
            const bool p2u = (tex->w & (tex->w - 1)) == 0, p2v = (tex->h & (tex->h - 1)) == 0;
            k->mu = (sm->wrap_u == ESPGPU_WRAP_REPEAT && p2u) ? (uint32_t)tex->w - 1u : ~0u;
            k->mv = (sm->wrap_v == ESPGPU_WRAP_REPEAT && p2v) ? (uint32_t)tex->h - 1u : ~0u;
            k->rep_u = sm->wrap_u != ESPGPU_WRAP_CLAMP;
            k->rep_v = sm->wrap_v != ESPGPU_WRAP_CLAMP;
            k->fpu = (float)(sm->wrap_u == ESPGPU_WRAP_MIRROR ? 2 * tex->w : tex->w);
            k->fpv = (float)(sm->wrap_v == ESPGPU_WRAP_MIRROR ? 2 * tex->h : tex->h);
            k->ifpu = 1.0f / k->fpu;
            k->ifpv = 1.0f / k->fpv;
            k->wlo_u = k->mu != ~0u ? -4096.0f : 0.0f;
            k->whi_u = k->mu != ~0u ? 4096.0f : k->fpu;
            k->wlo_v = k->mv != ~0u ? -4096.0f : 0.0f;
            k->whi_v = k->mv != ~0u ? 4096.0f : k->fpv;
        } else if (sh == ESPGPU_SHADE_TEX) {
            return -1; /* 无纹理的 TEX = 白色：少见，通用路径 */
        }
    }
    /* TEX_MOD_COLOR / FLAT：T × C（FLAT 与无纹理时 T = 白，gp_mul8(255, c) == c）。
     * 常量白色（2D 精灵、COLOR 255）：gp_mul8(t, 255) == t，省掉逐像素 4 次乘法 */
    const uint8_t *cc = t->const_rgba;
    k->ccol = (t->o_col < 0 || (t->cflags & GP_TC_COLOR)) ? 1 : 0;
    k->mod = sh != ESPGPU_SHADE_TEX &&
             !(kind != PK_NONE && k->ccol && cc[0] == 255 && cc[1] == 255 && cc[2] == 255 &&
               cc[3] == 255);
    k->crb = ((uint32_t)cc[0] << 24) | ((uint32_t)cc[2] << 8) | 0x00800080u;
    k->cga = ((uint32_t)cc[1] << 24) | ((uint32_t)cc[3] << 8) | 0x00800080u;
    k->cr = cc[0];
    k->cg = cc[1];
    k->cb = cc[2];
    k->ca = cc[3];
    k->cgray = (cc[0] == cc[1] && cc[1] == cc[2]) ? 1 : 0;

    if (s->blend) {
        if (s->bop != ESPGPU_BLEND_OP_ADD) {
            return -1;
        }
        const uint8_t bs = s->bsrc, bd = s->bdst;
        if (bs == ESPGPU_BLEND_SRC_ALPHA && bd == ESPGPU_BLEND_ONE_MINUS_SRC_ALPHA) {
            k->blend = PB_ALPHA;
        } else if (bs == ESPGPU_BLEND_ONE && bd == ESPGPU_BLEND_ONE) {
            k->blend = PB_ADD;
        } else if (bs == ESPGPU_BLEND_SRC_ALPHA && bd == ESPGPU_BLEND_ONE) {
            k->blend = PB_ADDA;
        } else if ((bs == ESPGPU_BLEND_DST_COLOR && bd == ESPGPU_BLEND_ZERO) ||
                   (bs == ESPGPU_BLEND_ZERO && bd == ESPGPU_BLEND_SRC_COLOR)) {
            k->blend = PB_MUL;
        } else if (bs == ESPGPU_BLEND_ONE && bd == ESPGPU_BLEND_ZERO) {
            k->blend = PB_NONE; /* 等价于不混合 */
        } else {
            return -1;
        }
    }
    k->fog = (s->fog && t->o_fog >= 0) ? 1 : 0;
    k->fog_rb = ((uint32_t)s->fog_r << 16) | s->fog_b;
    k->fog_g = s->fog_g;
    if (k->fog) {
        /* 雾表每个 worker 一份，雾色变了才重建（256 项，Doom 一整关只有一种雾色） */
        const uint32_t key = 0x80000000u | ((uint32_t)s->fog_r << 16) | ((uint32_t)s->fog_g << 8) |
                             s->fog_b;
        if (w->fog_key != key) {
            for (uint32_t f = 0; f < 256u; f++) {
                w->fog_rb_tab[f] = mul_rb(k->fog_rb, 255u - f);
                w->fog_g_tab[f] = (uint8_t)gp_mul8(k->fog_g, 255u - f);
            }
            w->fog_key = key;
        }
        k->fog_rbt = w->fog_rb_tab;
        k->fog_gt = w->fog_g_tab;
    }
    k->ztest = s->depth_test;
    k->zwrite = s->depth_write;
    k->zany = (k->ztest || k->zwrite) ? 1 : 0;
    cmpf_init(&k->zf, s->depth_func);
    k->atest = s->alpha_test;
    cmpf_init(&k->af, s->alpha_func);
    k->aref = s->alpha_ref;
    k->need_a = (k->atest || k->blend == PB_ALPHA || k->blend == PB_ADDA) ? 1 : 0;
    return kind;
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
#ifdef GP_TEST_NO_FAST
    return false; /* 测试用：与通用路径逐像素对比 */
#endif
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
    if (tex == NULL || s->smp[0].filter != ESPGPU_FILTER_NEAREST || !(t->flags & GP_TF_TEX0) ||
        (t->flags & GP_TF_TEX1) || t->n_att != 2) {
        return false;
    }
    /* RGB565；或 INDEX8 + 调色板且调色板不透明（没有 alpha 时 alpha 不影响结果） */
    return tex->px != NULL ||
           (tex->idx != NULL && tex->fmt != ESPGPU_FMT_R8 && tex->pal != NULL && tex->pal_a == NULL);
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

/**
 * 一个 64x64 tile 的全部光栅（fill / fast / px / generic 路径都内联在里面）。
 * 整个函数放 IRAM：它原来在 flash，内层循环在 240 MHz 下被取指拖慢 —— 实测
 * 只换这一个属性，Doom 静止 12.8 → 14.9 fps（tile 110 → 91 ms，pixel 64 → 54 ms），
 * 说明 13 KB 的函数 + 逐像素循环把 flash 指令 cache 压得很厉害。
 */
GP_HOT static void raster_tile(gpu_core_t *c, int worker, int tile)
{
    gp_bin_t *b = &c->bins[tile];
    if (b->count == 0) {
        return;
    }
    const uint32_t cy_start = gp_cycles();
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

    const uint32_t cy_io0 = gp_cycles();
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
    const uint32_t cy_io1 = gp_cycles();

    /* OVERDRAW 要逐片元计数，只有通用路径做这件事 */
    const bool count_frags = tc.dbg && tc.dbg_mode == ESPGPU_DEBUG_VIEW_OVERDRAW;
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
            pxs_t pk;
            int kind;
            if (!count_frags && state_is_fill(t, s)) {
                raster_tri_fill(&tc, t);
                w->n_path[GP_PATH_FILL]++;
            } else if (!count_frags && state_is_fast(t, s)) {
                raster_tri_fast(&tc, t, s);
                w->n_path[GP_PATH_FAST]++;
            } else if (!count_frags && (kind = px_setup(t, s, &pk, w)) >= 0) {
                raster_tri_px(&tc, t, &pk, kind);
                w->n_path[GP_PATH_PX]++;
            } else {
                raster_tri_generic(&tc, t, s);
                w->n_path[GP_PATH_GENERIC]++;
            }
            if (tc.dbg && tc.dbg_mode == ESPGPU_DEBUG_VIEW_WIREFRAME) {
                mark_wire(&tc, t);
            }
        }
        ch_off = ch->next;
    }

    const uint32_t cy_io2 = gp_cycles();
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
    const uint32_t cy_end = gp_cycles();
    /* 扫描记录的第一遍算进光栅；取入 + 写回算 I/O */
    w->cy_io += (cy_io1 - cy_io0) + (cy_end - cy_io2);
    w->cy_raster += (cy_io0 - cy_start) + (cy_io2 - cy_io1);
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
        gp_worker_t *w = &c->workers[i];
        w->fragments = 0;
        w->cy_raster = w->cy_io = 0;
        w->cy_px_seg = w->cy_px_pix = 0;
        w->n_px_rows = w->n_px_segs = w->n_px_tested = 0;
        memset(w->n_path, 0, sizeof(w->n_path));
    }
    if (c->nworkers > 1) {
        gp_parallel(tile_job, c);
    } else {
        tile_job(c, 0);
    }
    uint32_t f = 0;
    for (int i = 0; i < c->nworkers; i++) {
        const gp_worker_t *w = &c->workers[i];
        f += w->fragments;
        c->cy_raster += w->cy_raster;
        c->cy_io += w->cy_io;
        c->cy_px_seg += w->cy_px_seg;
        c->cy_px_pix += w->cy_px_pix;
        c->st_px_rows += w->n_px_rows;
        c->st_px_segs += w->n_px_segs;
        c->st_px_tested += w->n_px_tested;
        for (int k = 0; k < 4; k++) {
            c->st_path[k] += w->n_path[k];
        }
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
