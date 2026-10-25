/*
 * tnl.c — 顶点取数、变换与光照、裁剪、图元装配、三角形建立与分箱
 *
 * 流程：DRAW → 按索引取顶点（64 项后变换缓存）→ MVP / 光照 / 纹理矩阵 / 雾
 *       → 齐次裁剪（近/远 + 带保护带的 x/y）→ 透视除法 + 视口
 *       → 剔除 → 边函数 + 属性平面 → 按包围盒分进 tile。
 *
 * 浮点：S3 有单精度 FPU，每帧几千个顶点，全用 float 最省事也最准。
 */

#include <float.h>
#include <math.h>

#include "core_internal.h"


/* ── 属性解码 ─────────────────────────────────────────────────────── */

static inline float rd_f32(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

/**
 * 归一化 / 定点缩放一律乘倒数（S3 的 FPU 没有除法指令，除法是十几条指令的迭代序列），
 * 与除法最多差 1 ulp。scale 每次绘制按属性预先算好（fetch_t）。
 */
typedef struct {
    const espgpu_vertex_attrib_t *a;
    float scale;    /* 非归一化整数的定点缩放 2^-frac_bits */
    uint8_t n;      /* 分量数 1..4 */
} fetch_t;

static void fetch_init(fetch_t *f, const espgpu_vertex_attrib_t *a)
{
    f->a = a;
    f->n = (uint8_t)(a->components < 1 ? 1 : (a->components > 4 ? 4 : a->components));
    f->scale = a->frac_bits ? 1.0f / (float)(1u << (a->frac_bits & 31)) : 1.0f;
}

GP_HOT static void fetch_attr(const uint8_t *base, const fetch_t *f, float out[4])
{
    const espgpu_vertex_attrib_t *a = f->a;
    const uint8_t *p = base + a->offset;
    const int n = f->n;
    const float scale = f->scale;

    switch (a->type) {
    case ESPGPU_TYPE_F32:
        for (int i = 0; i < n; i++) out[i] = rd_f32(p + i * 4);
        break;
    case ESPGPU_TYPE_I8:
        for (int i = 0; i < n; i++) {
            const int8_t v = (int8_t)p[i];
            out[i] = a->normalized ? (v < -127 ? -1.0f : (float)v * (1.0f / 127.0f)) : (float)v * scale;
        }
        break;
    case ESPGPU_TYPE_U8:
        for (int i = 0; i < n; i++) {
            out[i] = a->normalized ? (float)p[i] * (1.0f / 255.0f) : (float)p[i] * scale;
        }
        break;
    case ESPGPU_TYPE_I16:
        for (int i = 0; i < n; i++) {
            int16_t v;
            memcpy(&v, p + i * 2, 2);
            out[i] = a->normalized ? (v < -32767 ? -1.0f : (float)v * (1.0f / 32767.0f))
                                   : (float)v * scale;
        }
        break;
    case ESPGPU_TYPE_U16:
        for (int i = 0; i < n; i++) {
            uint16_t v;
            memcpy(&v, p + i * 2, 2);
            out[i] = a->normalized ? (float)v * (1.0f / 65535.0f) : (float)v * scale;
        }
        break;
    case ESPGPU_TYPE_I32:
        for (int i = 0; i < n; i++) {
            int32_t v;
            memcpy(&v, p + i * 4, 4);
            out[i] = a->normalized ? (float)v * (1.0f / 2147483647.0f) : (float)v * scale;
        }
        break;
    case ESPGPU_TYPE_U32:
        for (int i = 0; i < n; i++) {
            uint32_t v;
            memcpy(&v, p + i * 4, 4);
            out[i] = a->normalized ? (float)v * (1.0f / 4294967295.0f) : (float)v * scale;
        }
        break;
    case ESPGPU_TYPE_FIXED16_16:
        for (int i = 0; i < n; i++) {
            int32_t v;
            memcpy(&v, p + i * 4, 4);
            out[i] = (float)v * (1.0f / 65536.0f);
        }
        break;
    case ESPGPU_TYPE_RGB565: {
        uint16_t v;
        memcpy(&v, p, 2);
        out[0] = (float)((v >> 11) & 0x1F) * (1.0f / 31.0f);
        out[1] = (float)((v >> 5) & 0x3F) * (1.0f / 63.0f);
        out[2] = (float)(v & 0x1F) * (1.0f / 31.0f);
        out[3] = 1.0f;
        break;
    }
    case ESPGPU_TYPE_RGBA4444: {
        uint16_t v;
        memcpy(&v, p, 2);
        out[0] = (float)((v >> 12) & 0xF) * (1.0f / 15.0f);
        out[1] = (float)((v >> 8) & 0xF) * (1.0f / 15.0f);
        out[2] = (float)((v >> 4) & 0xF) * (1.0f / 15.0f);
        out[3] = (float)(v & 0xF) * (1.0f / 15.0f);
        break;
    }
    default:
        break;
    }
}

/** fetch_attr 会读取的字节数（相对 a->offset） */
static uint32_t attr_bytes(const espgpu_vertex_attrib_t *a)
{
    const uint32_t n = a->components < 1 ? 1u : (a->components > 4 ? 4u : a->components);
    switch (a->type) {
    case ESPGPU_TYPE_I8:
    case ESPGPU_TYPE_U8: return n;
    case ESPGPU_TYPE_I16:
    case ESPGPU_TYPE_U16: return n * 2;
    case ESPGPU_TYPE_I32:
    case ESPGPU_TYPE_U32:
    case ESPGPU_TYPE_F32:
    case ESPGPU_TYPE_FIXED16_16: return n * 4;
    case ESPGPU_TYPE_RGB565:
    case ESPGPU_TYPE_RGBA4444: return 2;
    default: return 0;
    }
}

static inline void mat_vec4(const float *m, const float v[4], float o[4])
{
    o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3] * v[3];
    o[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7] * v[3];
    o[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11] * v[3];
    o[3] = m[12] * v[0] + m[13] * v[1] + m[14] * v[2] + m[15] * v[3];
}

/* ── 每次绘制的上下文 ─────────────────────────────────────────────── */

typedef struct {
    gpu_core_t *c;
    const uint8_t *vdata;
    uint32_t vavail;    /* 可用顶点数 */
    uint32_t stride;
    const espgpu_vertex_attrib_t *attr[8]; /* 按语义槽，0..6 */
    fetch_t fetch[8];   /* attr[] 非空的槽的解码参数 */
    gp_olayout_t ol;
    bool lighting;
    bool fog;
    bool vm;
    uint8_t varyings;
    bool flat;
    bool cull;
    float gx, gy;       /* 保护带系数 */
    /* 每次绘制预先算好的常量（顶点循环里不再除法） */
    float ccol[4];      /* 常量顶点色 /255 */
    float vsx, vox, vsy, voy; /* 窗口 x = vox + ndc_x * vsx，y 同理（y 向下） */
    float zs, zo;       /* 窗口 z（0..65535 尺度，含偏移）= zo + ndc_z * zs */
    float fog_inv_span; /* 线性雾 1/(end - start) */
} draw_ctx_t;

static bool shade_uses(uint8_t shade, int what) /* what: 0 = 颜色, 1 = T0, 2 = T1 */
{
    switch (shade) {
    case ESPGPU_SHADE_FLAT: return what == 0;
    case ESPGPU_SHADE_TEX: return what == 1;
    case ESPGPU_SHADE_TEX_MOD_COLOR:
    case ESPGPU_SHADE_TEX_ADD:
    case ESPGPU_SHADE_TEX_DECAL: return what <= 1;
    case ESPGPU_SHADE_TEX_MOD_TEX:
    case ESPGPU_SHADE_TEX_ADD_TEX: return true;
    case ESPGPU_SHADE_TEX_MOD_ADD_COLOR: return true;
    default: return what == 0;
    }
}

static void build_olayout(draw_ctx_t *dc)
{
    gpu_core_t *c = dc->c;
    gp_olayout_t *ol = &dc->ol;
    memset(ol, 0, sizeof(*ol));
    ol->off_color = ol->off_t0 = ol->off_t1 = ol->off_fog = -1;

    uint8_t shade = c->ps.shade_mode;
    if (shade == ESPGPU_SHADE_PROGRAM) {
        const gp_prog_t *p =
            c->ext.program_id < ESPGPU_VM_MAX_PROGRAMS ? c->progs[c->ext.program_id] : NULL;
        if (p && p->valid) {
            dc->vm = true;
            dc->varyings = p->varyings > 6 ? 6 : p->varyings;
            ol->flags |= GP_TF_VM;
            ol->n = (uint8_t)(dc->varyings * 4);
        } else {
            shade = ESPGPU_SHADE_FLAT;
        }
    }

    if (!dc->vm) {
        if (shade_uses(shade, 0)) {
            if (dc->attr[ESPGPU_ATTR_COLOR] == NULL && !dc->lighting) {
                ol->flags |= GP_TF_CONST_COLOR;
            } else {
                ol->off_color = (int8_t)ol->n;
                ol->n += 4;
            }
        }
        for (int u = 0; u < 2; u++) {
            if (!shade_uses(shade, 1 + u)) {
                continue;
            }
            const gp_tex_t *t = c->bind_id[u] ? gp_tex_find(&c->tc, c->bind_id[u]) : NULL;
            if (t == NULL || (t->px == NULL && t->idx == NULL)) {
                continue; /* 采样结果为白，不需要坐标 */
            }
            if (u == 0) {
                ol->off_t0 = (int8_t)ol->n;
                ol->t0_scale[0] = (float)t->w;
                ol->t0_scale[1] = (float)t->h;
                ol->flags |= GP_TF_TEX0;
            } else {
                ol->off_t1 = (int8_t)ol->n;
                ol->t1_scale[0] = (float)t->w;
                ol->t1_scale[1] = (float)t->h;
                ol->flags |= GP_TF_TEX1;
            }
            ol->n += 2;
        }
    }
    if (dc->fog) {
        ol->off_fog = (int8_t)ol->n;
        ol->n += 1;
        ol->flags |= GP_TF_FOG;
    }
}

static float fog_factor(const draw_ctx_t *dc, float dist)
{
    const gpu_core_t *c = dc->c;
    float f;
    switch (c->ps.fog_mode) {
    case ESPGPU_FOG_EXP:
        f = expf(-c->fog.fog_density * dist);
        break;
    case ESPGPU_FOG_EXP2: {
        const float d = c->fog.fog_density * dist;
        f = expf(-d * d);
        break;
    }
    default: {
        f = dc->fog_inv_span != 0.0f ? (c->fog.fog_end - dist) * dc->fog_inv_span : 1.0f;
        break;
    }
    }
    return gp_clampf(f, 0.0f, 1.0f);
}

/* ── 裁剪平面 ─────────────────────────────────────────────────────── */

#define N_PLANES 7

static inline float plane_dist(const draw_ctx_t *dc, int p, const float *v)
{
    switch (p) {
    case 0: return v[2] + v[3];              /* 近 */
    case 1: return v[3] - v[2];              /* 远 */
    case 2: return dc->gx * v[3] + v[0];     /* 左 */
    case 3: return dc->gx * v[3] - v[0];     /* 右 */
    case 4: return dc->gy * v[3] + v[1];     /* 下 */
    case 5: return dc->gy * v[3] - v[1];     /* 上 */
    default: return v[3] - 1e-5f;            /* w > 0 */
    }
}

static inline uint32_t outcode(const draw_ctx_t *dc, const float *v)
{
    /* 展开写：与 plane_dist 的 7 个平面逐一对应 */
    const float gw = dc->gx * v[3], hw = dc->gy * v[3];
    uint32_t m = 0;
    m |= (v[2] + v[3] < 0.0f) ? 1u : 0u;
    m |= (v[3] - v[2] < 0.0f) ? 2u : 0u;
    m |= (gw + v[0] < 0.0f) ? 4u : 0u;
    m |= (gw - v[0] < 0.0f) ? 8u : 0u;
    m |= (hw + v[1] < 0.0f) ? 16u : 0u;
    m |= (hw - v[1] < 0.0f) ? 32u : 0u;
    m |= (v[3] - 1e-5f < 0.0f) ? 64u : 0u;
    /* NaN：比较全为假会被当成在内侧，投影后的坐标再由 gp_setup_tri 的有限性检查丢掉 */
    return m;
}

/* ── 投影 ─────────────────────────────────────────────────────────── */

/** clip → 窗口坐标（x, y, z, iw），属性不动 */
static inline void project_xyz(const draw_ctx_t *dc, const float *clip, gp_wvtx_t *w)
{
    const float iw = 1.0f / clip[3];
    w->x = dc->vox + clip[0] * iw * dc->vsx;
    w->y = dc->voy + clip[1] * iw * dc->vsy;
    const float z = dc->zo + clip[2] * iw * dc->zs;
    w->z = gp_clampf(z, 0.0f, 65535.0f);
    w->iw = iw;
}

/* ── 顶点 ─────────────────────────────────────────────────────────── */

/** 处理一个顶点：裁剪坐标、outcode、属性，w > 0 时顺带投影 */
static void process_vertex(draw_ctx_t *dc, uint32_t index, gp_vce_t *o)
{
    gpu_core_t *c = dc->c;
    const uint8_t *base = dc->vdata + (size_t)index * dc->stride;

    float pos[4] = {0, 0, 0, 1};
    float col[4];
    float tc[4][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
    float nrm[4] = {0, 0, 1, 0};

    fetch_attr(base, &dc->fetch[ESPGPU_ATTR_POSITION], pos);
    if (dc->attr[ESPGPU_ATTR_COLOR]) {
        /* GL：属性里没给的分量补 (0,0,0,1)，不是 (1,1,1,1) */
        col[0] = col[1] = col[2] = 0.0f;
        col[3] = 1.0f;
        fetch_attr(base, &dc->fetch[ESPGPU_ATTR_COLOR], col);
    } else {
        memcpy(col, dc->ccol, sizeof(col));
    }
    /* 非 VM 只用得到 T0 / T1 */
    const int ntc = dc->vm ? 4 : 2;
    for (int u = 0; u < ntc; u++) {
        if (dc->attr[ESPGPU_ATTR_TEXCOORD0 + u]) {
            fetch_attr(base, &dc->fetch[ESPGPU_ATTR_TEXCOORD0 + u], tc[u]);
        }
    }
    if (dc->attr[ESPGPU_ATTR_NORMAL] && (dc->lighting || dc->vm)) {
        nrm[0] = nrm[1] = nrm[2] = 0.0f;
        nrm[3] = 1.0f;
        fetch_attr(base, &dc->fetch[ESPGPU_ATTR_NORMAL], nrm);
    }

    {
        const float *m = c->mat[ESPGPU_MATRIX_MVP];
        o->clip[0] = m[0] * pos[0] + m[1] * pos[1] + m[2] * pos[2] + m[3] * pos[3];
        o->clip[1] = m[4] * pos[0] + m[5] * pos[1] + m[6] * pos[2] + m[7] * pos[3];
        o->clip[2] = m[8] * pos[0] + m[9] * pos[1] + m[10] * pos[2] + m[11] * pos[3];
        o->clip[3] = m[12] * pos[0] + m[13] * pos[1] + m[14] * pos[2] + m[15] * pos[3];
    }

    if (dc->lighting) {
        const float *nm = c->mat[ESPGPU_MATRIX_NORMAL];
        float n[3] = {
            nm[0] * nrm[0] + nm[1] * nrm[1] + nm[2] * nrm[2],
            nm[4] * nrm[0] + nm[5] * nrm[1] + nm[6] * nrm[2],
            nm[8] * nrm[0] + nm[9] * nrm[1] + nm[10] * nrm[2],
        };
        const float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (len2 > 1e-12f) {
            const float inv = 1.0f / sqrtf(len2);
            n[0] *= inv;
            n[1] *= inv;
            n[2] *= inv;
        }
        float lr = c->light_hdr.ambient[0];
        float lg = c->light_hdr.ambient[1];
        float lb = c->light_hdr.ambient[2];
        for (uint32_t i = 0; i < c->light_hdr.count; i++) {
            const espgpu_light_t *L = &c->lights[i];
            float d = n[0] * L->dir[0] + n[1] * L->dir[1] + n[2] * L->dir[2];
            if (d > 0.0f) {
                lr += d * L->diffuse[0];
                lg += d * L->diffuse[1];
                lb += d * L->diffuse[2];
            }
        }
        col[0] *= gp_clampf(lr, 0.0f, 1.0f);
        col[1] *= gp_clampf(lg, 0.0f, 1.0f);
        col[2] *= gp_clampf(lb, 0.0f, 1.0f);
    }

    /* VM 模式下 TEXCOORDn 是通用 varying：不做纹理矩阵，也不做投影除法 */
    for (int u = 0; u < 2 && !dc->vm; u++) {
        if (c->ext.tex_matrix & (1u << u)) {
            float t[4];
            mat_vec4(c->mat[u == 0 ? ESPGPU_MATRIX_TEXTURE0 : ESPGPU_MATRIX_TEXTURE1], tc[u], t);
            memcpy(tc[u], t, sizeof(t));
        }
        if (tc[u][3] != 1.0f && tc[u][3] != 0.0f) {
            const float iq = 1.0f / tc[u][3];
            tc[u][0] *= iq;
            tc[u][1] *= iq;
        }
    }

    const gp_olayout_t *ol = &dc->ol;
    float *a = o->w.a;
    if (dc->vm) {
        const float *src[6] = {col, tc[0], tc[1], tc[2], tc[3], nrm};
        for (int k = 0; k < dc->varyings; k++) {
            memcpy(&a[k * 4], src[k], 16);
        }
    } else {
        if (ol->off_color >= 0) {
            memcpy(&a[ol->off_color], col, 16);
        }
        if (ol->off_t0 >= 0) {
            a[ol->off_t0] = tc[0][0] * ol->t0_scale[0];
            a[ol->off_t0 + 1] = tc[0][1] * ol->t0_scale[1];
        }
        if (ol->off_t1 >= 0) {
            a[ol->off_t1] = tc[1][0] * ol->t1_scale[0];
            a[ol->off_t1 + 1] = tc[1][1] * ol->t1_scale[1];
        }
    }
    if (ol->off_fog >= 0) {
        float dist;
        if (c->mat_set[ESPGPU_MATRIX_MODELVIEW]) {
            const float *m = c->mat[ESPGPU_MATRIX_MODELVIEW];
            dist = m[8] * pos[0] + m[9] * pos[1] + m[10] * pos[2] + m[11] * pos[3];
        } else {
            dist = o->clip[3];
        }
        a[ol->off_fog] = fog_factor(dc, fabsf(dist));
    }

    o->oc = outcode(dc, o->clip);
    if (!(o->oc & (1u << 6))) {
        project_xyz(dc, o->clip, &o->w);
    }
}

GP_HOT static const gp_vce_t *get_vertex(draw_ctx_t *dc, uint32_t index)
{
    gpu_core_t *c = dc->c;
    gp_vce_t *e = &c->vcache[index & (GP_VCACHE - 1)];
    if (e->index != index) {
        const uint32_t t0 = gp_cycles();
        process_vertex(dc, index, e);
        e->index = index;
        c->st_verts++;
        c->cy_vtx += gp_cycles() - t0;
    }
    return e;
}

/* ── 裁剪 ─────────────────────────────────────────────────────────── */

/** 只拷贝本次绘制用到的分量（gp_cvtx_t 约 128 字节，属性通常只有几个） */
static inline void cvtx_from(const draw_ctx_t *dc, gp_cvtx_t *o, const float *clip, const float *a)
{
    memcpy(o->clip, clip, sizeof(o->clip));
    memcpy(o->a, a, sizeof(float) * dc->ol.n);
}

static void lerp_vtx(const draw_ctx_t *dc, const gp_cvtx_t *a, const gp_cvtx_t *b, float t,
                     gp_cvtx_t *o)
{
    for (int i = 0; i < 4; i++) {
        o->clip[i] = a->clip[i] + (b->clip[i] - a->clip[i]) * t;
    }
    for (int i = 0; i < dc->ol.n; i++) {
        o->a[i] = a->a[i] + (b->a[i] - a->a[i]) * t;
    }
}

/** Sutherland–Hodgman，返回输出顶点数（最多 3 + N_PLANES） */
GP_HOT static int clip_poly(const draw_ctx_t *dc, uint32_t mask, gp_cvtx_t *buf_a, gp_cvtx_t *buf_b,
                     int n, gp_cvtx_t **out)
{
    gp_cvtx_t *in = buf_a;
    gp_cvtx_t *res = buf_b;
    for (int p = 0; p < N_PLANES && n > 0; p++) {
        if (!(mask & (1u << p))) {
            continue;
        }
        int m = 0;
        for (int i = 0; i < n; i++) {
            const gp_cvtx_t *A = &in[i];
            const gp_cvtx_t *B = &in[(i + 1) % n];
            const float da = plane_dist(dc, p, A->clip);
            const float db = plane_dist(dc, p, B->clip);
            if (da >= 0.0f) {
                cvtx_from(dc, &res[m++], A->clip, A->a);
            }
            if ((da >= 0.0f) != (db >= 0.0f)) {
                const float t = da / (da - db);
                lerp_vtx(dc, A, B, t, &res[m++]);
            }
        }
        n = m;
        gp_cvtx_t *t = in;
        in = res;
        res = t;
    }
    *out = in;
    return n;
}

static void project(const draw_ctx_t *dc, const gp_cvtx_t *v, gp_wvtx_t *w)
{
    project_xyz(dc, v->clip, w);
    memcpy(w->a, v->a, sizeof(float) * dc->ol.n);
}

/* ── 三角形建立 ───────────────────────────────────────────────────── */

/**
 * v*16 就近取整（就近偶数，与 lrintf 逐位相同）。|v*16| < 2^22 时加减 1.5*2^23：
 * 两次 FPU 加法 + 截断，不走 lrintf 库函数（Xtensa 上没有对应指令，是软件实现）。
 * 要求 float 运算按 IEEE 单精度舍入（FLT_EVAL_METHOD 0：x64 / Xtensa / ARM 都是）。
 */
static inline int32_t snap16(float v)
{
    const float f = v * 16.0f;
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD == 0
    if (fabsf(f) < 4194304.0f) {
        const float m = 12582912.0f;
        const float r = (f + m) - m;
        return (int32_t)r;
    }
#endif
    return (int32_t)lrintf(f);
}

/** 平面系数的公共部分：dx = d1*A - d2*B，dy = d2*C - d1*D，c = v0 + dx*ox + dy*oy */
typedef struct {
    float A, B, C, D, ox, oy;
} plane_k_t;

static inline void make_plane(gp_plane_t *p, const plane_k_t *k, float v0, float v1, float v2)
{
    if (v0 == v1 && v0 == v2) {
        /* 常量属性（雾全 0 / 全 1、平面着色色、单色模型）：省掉乘法，结果严格是常量 */
        p->dx = p->dy = 0.0f;
        p->c = v0;
        return;
    }
    const float d1 = v1 - v0;
    const float d2 = v2 - v0;
    p->dx = d1 * k->A - d2 * k->B;
    p->dy = d2 * k->C - d1 * k->D;
    p->c = v0 + p->dx * k->ox + p->dy * k->oy;
}

/** 透视：对 a*iw 建平面；a 三顶点相同时直接是 a × (1/w 平面) */
static inline void make_plane_p(gp_plane_t *p, const plane_k_t *k, const gp_plane_t *iwp, float a0,
                                float a1, float a2, float w0, float w1, float w2)
{
    if (a0 == a1 && a0 == a2) {
        p->c = a0 * iwp->c;
        p->dx = a0 * iwp->dx;
        p->dy = a0 * iwp->dy;
        return;
    }
    make_plane(p, k, a0 * w0, a1 * w1, a2 * w2);
}

static inline bool unit01(float v)
{
    return v >= 0.0f && v <= 1.0f; /* NaN → false */
}

/**
 * 分箱：包围盒在两个方向都跨 tile 时，角上的 tile 可能完全在某条边外侧（细长斜三角形），
 * 先用边函数在 tile 矩形上的最大值剔掉，省 arena 和光栅时的一次记录读取。
 */
static void bin_tri(gpu_core_t *c, const gp_tri_t *t, uint32_t off)
{
    const int tx0 = t->bx0 >> GP_TILE_SHIFT, ty0 = t->by0 >> GP_TILE_SHIFT;
    const int tx1 = t->bx1 >> GP_TILE_SHIFT, ty1 = t->by1 >> GP_TILE_SHIFT;
    if (tx0 == tx1 || ty0 == ty1) {
        gp_bin_record(c, off, t->bx0, t->by0, t->bx1, t->by1);
        return;
    }
    for (int ty = ty0; ty <= ty1; ty++) {
        const int y0 = ty == ty0 ? t->by0 : ty << GP_TILE_SHIFT;
        const int y1 = ty == ty1 ? t->by1 : (ty << GP_TILE_SHIFT) + GP_TILE - 1;
        for (int tx = tx0; tx <= tx1; tx++) {
            const int x0 = tx == tx0 ? t->bx0 : tx << GP_TILE_SHIFT;
            const int x1 = tx == tx1 ? t->bx1 : (tx << GP_TILE_SHIFT) + GP_TILE - 1;
            bool out = false;
            for (int k = 0; k < 3 && !out; k++) {
                const int32_t a = t->ea[k], b = t->eb[k];
                const int64_t emax = (int64_t)a * (a > 0 ? x1 : x0) + (int64_t)b * (b > 0 ? y1 : y0) +
                                     t->ec[k];
                out = emax < 0;
            }
            if (!out) {
                gp_bin_tile(c, ty * c->tiles_x + tx, off);
            }
        }
    }
    c->pending = true;
}

GP_HOT bool gp_setup_tri(gpu_core_t *c, const gp_wvtx_t *v0, const gp_wvtx_t *v1, const gp_wvtx_t *v2,
                  const gp_olayout_t *ol, const float *flat_col, bool cull_enabled)
{
    const uint32_t cy0 = gp_cycles();
    const bool is2d = (ol->flags & GP_TF_2D) != 0;

    const gp_wvtx_t *V[3] = {v0, v1, v2};
    int32_t X[3], Y[3];
    for (int i = 0; i < 3; i++) {
        /* NaN / 极端坐标（坏矩阵、坏视口）：直接丢弃。±2^20 像素内边函数系数放得进 int32 */
        if (!(fabsf(V[i]->x) < 1048576.0f) || !(fabsf(V[i]->y) < 1048576.0f)) {
            c->cy_setup += gp_cycles() - cy0;
            return true;
        }
        X[i] = snap16(V[i]->x);
        Y[i] = snap16(V[i]->y);
    }
    /* |X|,|Y| < 2^24：差值放得进 int32，乘积用 32×32→64（Xtensa 上是 MULL + MULSH，不走 __muldi3） */
    int64_t area = (int64_t)(X[1] - X[0]) * (Y[2] - Y[0]) - (int64_t)(Y[1] - Y[0]) * (X[2] - X[0]);
    if (area == 0) {
        c->cy_setup += gp_cycles() - cy0;
        return true;
    }

    if (cull_enabled && c->ps.cull_mode != ESPGPU_CULL_NONE) {
        /* 窗口坐标 y 向下：面积为正 = NDC 里顺时针 */
        const bool front = (c->ps.front_face == ESPGPU_FRONT_CCW) ? (area < 0) : (area > 0);
        const uint8_t cm = c->ps.cull_mode;
        if (cm == ESPGPU_CULL_BOTH || (cm == ESPGPU_CULL_BACK && !front) ||
            (cm == ESPGPU_CULL_FRONT && front)) {
            c->st_culled++;
            c->cy_setup += gp_cycles() - cy0;
            return true;
        }
    }

    if (area < 0) {
        const gp_wvtx_t *t = V[1];
        V[1] = V[2];
        V[2] = t;
        int32_t tx = X[1]; X[1] = X[2]; X[2] = tx;
        int32_t ty = Y[1]; Y[1] = Y[2]; Y[2] = ty;
        area = -area;
    }

    /* 包围盒（像素中心 16p+8 落在 [min,max] 内）；>> 4 即向下取整（算术移位） */
    int32_t minx = X[0], maxx = X[0], miny = Y[0], maxy = Y[0];
    for (int i = 1; i < 3; i++) {
        if (X[i] < minx) minx = X[i];
        if (X[i] > maxx) maxx = X[i];
        if (Y[i] < miny) miny = Y[i];
        if (Y[i] > maxy) maxy = Y[i];
    }
    int bx0 = (minx - 8 + 15) >> 4;
    int by0 = (miny - 8 + 15) >> 4;
    int bx1 = (maxx - 8) >> 4;
    int by1 = (maxy - 8) >> 4;
    if (bx0 < c->sc_x) bx0 = c->sc_x;
    if (by0 < c->sc_y) by0 = c->sc_y;
    if (bx1 > c->sc_x + c->sc_w - 1) bx1 = c->sc_x + c->sc_w - 1;
    if (by1 > c->sc_y + c->sc_h - 1) by1 = c->sc_y + c->sc_h - 1;
    if (bx0 > bx1 || by0 > by1) {
        c->cy_setup += gp_cycles() - cy0;
        return true;
    }

    const uint32_t n_att = ol->n;
    const uint32_t rec_bytes = (uint32_t)(sizeof(gp_tri_t) + n_att * sizeof(gp_plane_t));
    const uint32_t n_tiles = (uint32_t)(((bx1 >> GP_TILE_SHIFT) - (bx0 >> GP_TILE_SHIFT) + 1) *
                                        ((by1 >> GP_TILE_SHIFT) - (by0 >> GP_TILE_SHIFT) + 1));
    if (!gp_reserve(c, rec_bytes, n_tiles)) {
        c->cy_setup += gp_cycles() - cy0;
        return false;
    }
    const uint32_t state_off = gp_snapshot_state(c, is2d);
    if (state_off == 0) {
        c->cy_setup += gp_cycles() - cy0;
        return false;
    }

    uint32_t off;
    gp_tri_t *t = (gp_tri_t *)gp_arena_alloc(c, rec_bytes, &off);
    if (t == NULL) {
        c->cy_setup += gp_cycles() - cy0;
        return false;
    }
    uint32_t flags = ol->flags & ~(uint32_t)GP_TF_2D;
    t->type = GP_REC_TRI;
    t->n_att = (uint8_t)n_att;
    t->cflags = 0;
    t->o_col = ol->off_color;
    t->o_t0 = ol->off_t0;
    t->o_t1 = ol->off_t1;
    t->o_fog = ol->off_fog;
    t->bx0 = (int16_t)bx0;
    t->by0 = (int16_t)by0;
    t->bx1 = (int16_t)bx1;
    t->by1 = (int16_t)by1;
    t->state_off = state_off;
    memcpy(t->const_rgba, c->const_color, 4);

    /* 边 k 对着顶点 k：e0 = v1→v2，e1 = v2→v0，e2 = v0→v1 */
    float fa[3];
    for (int k = 0; k < 3; k++) {
        const int i = (k + 1) % 3;
        const int j = (k + 2) % 3;
        const int32_t dx = X[j] - X[i];
        const int32_t dy = Y[j] - Y[i];
        const int32_t a = -16 * dy;
        const int32_t b = 16 * dx;
        int64_t cc = (int64_t)dx * (8 - Y[i]) - (int64_t)dy * (8 - X[i]);
        const bool top_left = (a > 0) || (a == 0 && b > 0);
        if (!top_left) {
            cc -= 1;
        }
        t->ea[k] = a;
        t->eb[k] = b;
        t->ec[k] = cc;
        fa[k] = a != 0 ? (float)a : 1.0f;
    }

    /* 1/ea[k] 与 256/area 共用一次除法：r = 1/(a0·a1·a2·area)，各倒数 = r × 其余因子之积。
     * inv_ea 只用作跨度估计（光栅时用精确整数边函数修正），inv_det 相对误差几个 ulp；
     * 乘积可能溢出 / 下溢的病态三角形退回逐个除 */
    const float farea = (float)area;
    const float p01 = fa[0] * fa[1];
    const float pa = p01 * fa[2];
    const float pall = pa * farea;
    float inv_det;
    if (pall > -1e36f && pall < 1e36f && (pall > 1e-30f || pall < -1e-30f)) {
        const float r = 1.0f / pall;
        const float ra = r * farea;
        t->inv_ea[0] = t->ea[0] ? ra * fa[1] * fa[2] : 0.0f;
        t->inv_ea[1] = t->ea[1] ? ra * fa[0] * fa[2] : 0.0f;
        t->inv_ea[2] = t->ea[2] ? ra * p01 : 0.0f;
        inv_det = 256.0f * r * pa;
    } else {
        for (int k = 0; k < 3; k++) {
            t->inv_ea[k] = t->ea[k] ? 1.0f / fa[k] : 0.0f;
        }
        inv_det = 256.0f / farea;
    }

    /* 平面 */
    const float x0 = (float)X[0] * (1.0f / 16.0f), y0 = (float)Y[0] * (1.0f / 16.0f);
    const float x1 = (float)X[1] * (1.0f / 16.0f), y1 = (float)Y[1] * (1.0f / 16.0f);
    const float x2 = (float)X[2] * (1.0f / 16.0f), y2 = (float)Y[2] * (1.0f / 16.0f);
    plane_k_t pk;
    pk.A = (y2 - y0) * inv_det;
    pk.B = (y1 - y0) * inv_det;
    pk.C = (x1 - x0) * inv_det;
    pk.D = (x2 - x0) * inv_det;
    pk.ox = 0.5f - x0;
    pk.oy = 0.5f - y0;

    make_plane(&t->z, &pk, V[0]->z, V[1]->z, V[2]->z);

    const float w0 = V[0]->iw, w1 = V[1]->iw, w2 = V[2]->iw;
    const float tol = fabsf(w0) * 1e-5f;
    const bool affine = fabsf(w1 - w0) <= tol && fabsf(w2 - w0) <= tol;
    const int oc = ol->off_color;
    if (affine) {
        flags |= GP_TF_AFFINE;
        t->iw.c = w0;
        t->iw.dx = t->iw.dy = 0.0f;
    } else {
        make_plane(&t->iw, &pk, w0, w1, w2);
    }
    for (uint32_t k = 0; k < n_att; k++) {
        float a0 = V[0]->a[k], a1 = V[1]->a[k], a2 = V[2]->a[k];
        if (flat_col && oc >= 0 && (int)k >= oc && (int)k < oc + 4) {
            a0 = a1 = a2 = flat_col[k - (uint32_t)oc];
        }
        if (affine) {
            make_plane(&t->att[k], &pk, a0, a1, a2);
        } else {
            make_plane_p(&t->att[k], &pk, &t->iw, a0, a1, a2, w0, w1, w2);
        }
    }

    /* 颜色三顶点都在 [0,1]（光照 / 归一化顶点色总是如此，F32 顶点色可能越界）：
     * 三角形内（凸组合）也在 [0,1]，像素管线快速路径可以只在段端点夹取 */
    bool unit = true;
    if (oc >= 0 && !(flags & GP_TF_VM)) {
        for (int i = 0; i < 3 && unit; i++) {
            const float *col = flat_col ? flat_col : &V[i]->a[oc];
            unit = unit01(col[0]) && unit01(col[1]) && unit01(col[2]) && unit01(col[3]);
        }
    }
    if (unit) {
        flags |= GP_TF_UNIT;
        /* 三个顶点颜色相同：光栅时直接用常量（像素管线每段少插值 4 个通道，见 GP_TC_COLOR）。
         * 舍入与 float_rgba 相同，通用路径和快速路径取到的是同一个字节 */
        if (oc >= 0 && !(flags & GP_TF_VM)) {
            const float *c0 = flat_col ? flat_col : &V[0]->a[oc];
            bool same = flat_col != NULL;
            if (!same) {
                const float *c1 = &V[1]->a[oc], *c2 = &V[2]->a[oc];
                same = c0[0] == c1[0] && c0[1] == c1[1] && c0[2] == c1[2] && c0[3] == c1[3] &&
                       c0[0] == c2[0] && c0[1] == c2[1] && c0[2] == c2[2] && c0[3] == c2[3];
            }
            if (same) {
                t->cflags |= GP_TC_COLOR;
                for (int i = 0; i < 4; i++) {
                    t->const_rgba[i] = (uint8_t)(c0[i] * 255.0f + 0.5f);
                }
            }
        }
    }
    t->flags = (uint8_t)flags;

    const uint32_t cy1 = gp_cycles();
    c->cy_setup += cy1 - cy0;
    bin_tri(c, t, off);
    c->cy_bin += gp_cycles() - cy1;
    c->st_tris++;
    return true;
}

/** 失败（arena 满）时刷新后重试一次 */
static void emit_tri(draw_ctx_t *dc, const gp_wvtx_t *a, const gp_wvtx_t *b, const gp_wvtx_t *c,
                     const float *flat, bool cull)
{
    if (!gp_setup_tri(dc->c, a, b, c, &dc->ol, flat, cull)) {
        gp_flush(dc->c);
        if (!gp_setup_tri(dc->c, a, b, c, &dc->ol, flat, cull)) {
            gp_log(GP_LOG_WARN, "triangle dropped: arena too small");
        }
    }
}

/* ── 图元 ─────────────────────────────────────────────────────────── */

/** prov = 激发顶点的索引（平面着色时取它的颜色） */
GP_HOT static void draw_triangle(draw_ctx_t *dc, uint32_t i0, uint32_t i1, uint32_t i2, uint32_t prov)
{
    float pcol[4];
    const float *flat = NULL;
    if (dc->flat && dc->ol.off_color >= 0) {
        /* 先取激发顶点的颜色：它和 i0..i2 可能落在同一个缓存槽 */
        memcpy(pcol, &get_vertex(dc, prov)->w.a[dc->ol.off_color], sizeof(pcol));
        flat = pcol;
    }

    const gp_vce_t *A = get_vertex(dc, i0);
    const gp_vce_t *B = get_vertex(dc, i1);
    const gp_vce_t *C = get_vertex(dc, i2);
    gp_cvtx_t va[3 + N_PLANES + 2];
    gp_cvtx_t vb[3 + N_PLANES + 2];

    if (A->index != i0 || B->index != i1) {
        /* 两个不同索引落进同一槽（相差 64 的倍数）：后取的把先取的冲掉了，按值重取 */
        A = get_vertex(dc, i0);
        cvtx_from(dc, &va[0], A->clip, A->w.a);
        const uint32_t oa = A->oc;
        B = get_vertex(dc, i1);
        cvtx_from(dc, &va[1], B->clip, B->w.a);
        const uint32_t ob = B->oc;
        C = get_vertex(dc, i2);
        cvtx_from(dc, &va[2], C->clip, C->w.a);
        const uint32_t oc = C->oc;
        if (oa & ob & oc) {
            dc->c->st_culled++;
            return;
        }
        dc->c->st_prims++;
        const uint32_t cy0 = gp_cycles();
        gp_cvtx_t *poly = va;
        int n = 3;
        if (oa | ob | oc) {
            n = clip_poly(dc, oa | ob | oc, va, vb, 3, &poly);
        }
        gp_wvtx_t w[3 + N_PLANES + 2];
        for (int i = 0; i < n; i++) {
            project(dc, &poly[i], &w[i]);
        }
        dc->c->cy_clip += gp_cycles() - cy0;
        for (int i = 1; i + 1 < n; i++) {
            emit_tri(dc, &w[0], &w[i], &w[i + 1], flat, true);
        }
        return;
    }

    const uint32_t o0 = A->oc, o1 = B->oc, o2 = C->oc;
    if (o0 & o1 & o2) {
        dc->c->st_culled++;
        return;
    }
    dc->c->st_prims++;

    const uint32_t any = o0 | o1 | o2;
    if (!any) {
        /* 常见情况：三个顶点都在保护带内，直接用缓存里已投影的窗口坐标 */
        emit_tri(dc, &A->w, &B->w, &C->w, flat, true);
        return;
    }

    const uint32_t cy0 = gp_cycles();
    cvtx_from(dc, &va[0], A->clip, A->w.a);
    cvtx_from(dc, &va[1], B->clip, B->w.a);
    cvtx_from(dc, &va[2], C->clip, C->w.a);
    gp_cvtx_t *poly = va;
    const int n = clip_poly(dc, any, va, vb, 3, &poly);
    if (n < 3) {
        dc->c->cy_clip += gp_cycles() - cy0;
        return;
    }
    gp_wvtx_t w[3 + N_PLANES + 2];
    for (int i = 0; i < n; i++) {
        project(dc, &poly[i], &w[i]);
    }
    dc->c->cy_clip += gp_cycles() - cy0;
    /* 扇形三角化；裁剪不改变绕序，剔除判断仍然有效 */
    for (int i = 1; i + 1 < n; i++) {
        emit_tri(dc, &w[0], &w[i], &w[i + 1], flat, true);
    }
}

/** 屏幕空间四边形（线 / 点展开用） */
static void emit_quad(draw_ctx_t *dc, const gp_wvtx_t *p0, const gp_wvtx_t *p1, float nx, float ny,
                      float ex, float ey)
{
    gp_wvtx_t q[4];
    q[0] = *p0;
    q[1] = *p0;
    q[2] = *p1;
    q[3] = *p1;
    q[0].x += nx - ex; q[0].y += ny - ey;
    q[1].x -= nx + ex; q[1].y -= ny + ey;
    q[2].x -= nx - ex; q[2].y -= ny - ey;
    q[3].x += nx + ex; q[3].y += ny + ey;
    emit_tri(dc, &q[0], &q[1], &q[2], NULL, false);
    emit_tri(dc, &q[0], &q[2], &q[3], NULL, false);
}

static void draw_line(draw_ctx_t *dc, uint32_t i0, uint32_t i1)
{
    gp_cvtx_t a, b;
    const gp_vce_t *e = get_vertex(dc, i0);
    cvtx_from(dc, &a, e->clip, e->w.a);
    e = get_vertex(dc, i1);
    cvtx_from(dc, &b, e->clip, e->w.a);
    if (dc->flat && dc->ol.off_color >= 0) {
        memcpy(&a.a[dc->ol.off_color], &b.a[dc->ol.off_color], 16);
    }
    const uint32_t oa = outcode(dc, a.clip);
    const uint32_t ob = outcode(dc, b.clip);
    if (oa & ob) {
        return;
    }
    float t0 = 0.0f, t1 = 1.0f;
    for (int p = 0; p < N_PLANES; p++) {
        if (!((oa | ob) & (1u << p))) {
            continue;
        }
        const float da = plane_dist(dc, p, a.clip);
        const float db = plane_dist(dc, p, b.clip);
        if (da < 0 && db < 0) {
            return;
        }
        const float t = da / (da - db);
        if (da < 0) {
            if (t > t0) t0 = t;
        } else if (db < 0) {
            if (t < t1) t1 = t;
        }
    }
    if (t0 >= t1) {
        return;
    }
    gp_cvtx_t ca, cb;
    lerp_vtx(dc, &a, &b, t0, &ca);
    lerp_vtx(dc, &a, &b, t1, &cb);
    gp_wvtx_t wa, wb;
    project(dc, &ca, &wa);
    project(dc, &cb, &wb);

    float dx = wb.x - wa.x;
    float dy = wb.y - wa.y;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) {
        return;
    }
    const float hw = (dc->c->ext.line_width ? (float)dc->c->ext.line_width : 1.0f) * 0.5f;
    dx /= len;
    dy /= len;
    /* 法向偏移半个线宽，长度方向不延长：配合 top-left 规则得到 GL 的半开线段
     * （起点像素画、终点像素不画），线带的连接点不会被画两次（混合时可见） */
    emit_quad(dc, &wa, &wb, -dy * hw, dx * hw, 0.0f, 0.0f);
    dc->c->st_prims++;
}

static void draw_point(draw_ctx_t *dc, uint32_t i0)
{
    const gp_vce_t *v = get_vertex(dc, i0);
    if (v->oc) {
        return;
    }
    gp_wvtx_t w;
    w = v->w;
    const float hs = (dc->c->ext.point_size ? (float)dc->c->ext.point_size : 1.0f) * 0.5f;
    emit_quad(dc, &w, &w, 0.0f, hs, hs, 0.0f);
    dc->c->st_prims++;
}

/* ── DRAW ─────────────────────────────────────────────────────────── */

GP_HOT void gp_draw3d(gpu_core_t *c, const espgpu_draw_t *d, const uint8_t *inline_data,
               uint32_t inline_len)
{
    if (!c->layout_valid) {
        return;
    }

    draw_ctx_t dc;
    memset(&dc, 0, sizeof(dc));
    dc.c = c;
    dc.stride = c->layout.stride;
    uint32_t extent = 0; /* 一个顶点实际读到的最远字节（属性可以越过 stride） */
    for (int i = 0; i < c->layout.attrib_count; i++) {
        const espgpu_vertex_attrib_t *a = &c->layout.attribs[i];
        if (a->slot < 8) {
            dc.attr[a->slot] = a;
        }
    }
    for (int s = 0; s <= ESPGPU_ATTR_TEXCOORD3; s++) { /* fetch_attr 只读这些槽 */
        if (dc.attr[s]) {
            const uint32_t e = (uint32_t)dc.attr[s]->offset + attr_bytes(dc.attr[s]);
            if (e > extent) extent = e;
        }
    }
    if (dc.attr[ESPGPU_ATTR_POSITION] == NULL) {
        return;
    }

    const unsigned isz = c->layout.index_type == 2 ? 4u : 2u;
    const uint8_t *indices = NULL;
    uint64_t vbytes_avail = 0;

    if (d->source == ESPGPU_VSRC_BUFFER) {
        uint32_t blen = 0;
        const uint8_t *b = gp_buffer_data(c, d->buffer_id, &blen);
        if (b == NULL) {
            return;
        }
        const uint64_t vstart = (uint64_t)d->base_vertex * dc.stride;
        if (vstart > blen) {
            return;
        }
        dc.vdata = b + vstart;
        dc.vavail = (uint32_t)((blen - vstart) / dc.stride);
        vbytes_avail = blen - vstart;
        if (d->index_count) {
            if ((uint64_t)d->index_offset + (uint64_t)d->index_count * isz > blen) {
                return;
            }
            indices = b + d->index_offset;
        }
    } else {
        const uint64_t vbytes = (uint64_t)d->vertex_count * dc.stride;
        if (vbytes > inline_len) {
            return;
        }
        dc.vdata = inline_data;
        dc.vavail = d->vertex_count;
        vbytes_avail = inline_len;
        if (d->index_count) {
            if (vbytes + (uint64_t)d->index_count * isz > inline_len) {
                return;
            }
            indices = inline_data + vbytes;
        }
    }
    /* 顶点 i 读 [i*stride, i*stride + extent)，不能越过数据末尾 */
    if (extent > dc.stride) {
        const uint32_t fit = vbytes_avail >= extent
                                 ? (uint32_t)((vbytes_avail - extent) / dc.stride) + 1u
                                 : 0u;
        if (fit < dc.vavail) {
            dc.vavail = fit;
        }
    }

    dc.lighting = c->ext.lighting && c->light_hdr.enable;
    dc.fog = c->ps.fog_enable != 0;
    dc.flat = c->ext.shade_model == ESPGPU_SHADE_MODEL_FLAT;
    dc.gx = 1.0f + 2.0f * GP_GUARD_PX / (float)(c->vp_w > 0 ? c->vp_w : 1);
    dc.gy = 1.0f + 2.0f * GP_GUARD_PX / (float)(c->vp_h > 0 ? c->vp_h : 1);
    build_olayout(&dc);
    for (int s = 0; s < 8; s++) {
        if (dc.attr[s]) {
            fetch_init(&dc.fetch[s], dc.attr[s]);
        }
    }
    for (int i = 0; i < 4; i++) {
        dc.ccol[i] = (float)c->const_color[i] * (1.0f / 255.0f);
    }
    /* 视口：x = vp_x + (ndc+1)/2 · w = (vp_x + w/2) + ndc · w/2；y 向下 */
    dc.vsx = 0.5f * (float)c->vp_w;
    dc.vox = (float)c->vp_x + dc.vsx;
    dc.vsy = -0.5f * (float)c->vp_h;
    dc.voy = (float)c->vp_y - dc.vsy;
    /* z = (n + (ndc+1)/2 · (f-n)) · 65535 - bias */
    dc.zs = 0.5f * (c->depth_f - c->depth_n) * 65535.0f;
    dc.zo = c->depth_n * 65535.0f + dc.zs - (float)c->ext.depth_bias;
    {
        const float span = c->fog.fog_end - c->fog.fog_start;
        dc.fog_inv_span = span != 0.0f ? 1.0f / span : 0.0f;
    }

    /* 后变换缓存每次绘制失效（矩阵 / 布局可能已变） */
    for (int i = 0; i < GP_VCACHE; i++) {
        c->vcache[i].index = 0xFFFFFFFFu;
    }

    const uint32_t count = d->index_count ? d->index_count : d->vertex_count;
#define VALID(i) ((i) < dc.vavail)

    /* 索引可能不对齐，统一走 memcpy */
    uint32_t idx_buf[4];
#define LOADI(k)                                                              \
    (indices ? (isz == 2 ? (memcpy(&idx_buf[0], indices + (size_t)(k) * 2, 2), \
                            (uint32_t)(uint16_t)idx_buf[0])                   \
                         : (memcpy(&idx_buf[0], indices + (size_t)(k) * 4, 4), idx_buf[0])) \
             : (uint32_t)(k))
    (void)idx_buf;

    switch (d->prim) {
    case ESPGPU_PRIM_TRIANGLES:
        for (uint32_t k = 0; k + 2 < count; k += 3) {
            uint32_t a = LOADI(k), b = LOADI(k + 1), e = LOADI(k + 2);
            if (VALID(a) && VALID(b) && VALID(e)) draw_triangle(&dc, a, b, e, e);
        }
        break;
    case ESPGPU_PRIM_TRIANGLE_STRIP:
        for (uint32_t k = 0; k + 2 < count; k++) {
            uint32_t a = LOADI(k), b = LOADI(k + 1), e = LOADI(k + 2);
            if (!(VALID(a) && VALID(b) && VALID(e))) continue;
            if (k & 1) draw_triangle(&dc, b, a, e, e);
            else draw_triangle(&dc, a, b, e, e);
        }
        break;
    case ESPGPU_PRIM_TRIANGLE_FAN: {
        if (count < 3) break;
        const uint32_t a = LOADI(0);
        for (uint32_t k = 1; k + 1 < count; k++) {
            uint32_t b = LOADI(k), e = LOADI(k + 1);
            if (VALID(a) && VALID(b) && VALID(e)) draw_triangle(&dc, a, b, e, e);
        }
        break;
    }
    case ESPGPU_PRIM_QUADS:
        for (uint32_t k = 0; k + 3 < count; k += 4) {
            uint32_t a = LOADI(k), b = LOADI(k + 1), e = LOADI(k + 2), f = LOADI(k + 3);
            if (!(VALID(a) && VALID(b) && VALID(e) && VALID(f))) continue;
            /* GL：四边形的激发顶点是第 4 个 */
            draw_triangle(&dc, a, b, e, f);
            draw_triangle(&dc, a, e, f, f);
        }
        break;
    case ESPGPU_PRIM_LINES:
        for (uint32_t k = 0; k + 1 < count; k += 2) {
            uint32_t a = LOADI(k), b = LOADI(k + 1);
            if (VALID(a) && VALID(b)) draw_line(&dc, a, b);
        }
        break;
    case ESPGPU_PRIM_LINE_STRIP:
    case ESPGPU_PRIM_LINE_LOOP:
        for (uint32_t k = 0; k + 1 < count; k++) {
            uint32_t a = LOADI(k), b = LOADI(k + 1);
            if (VALID(a) && VALID(b)) draw_line(&dc, a, b);
        }
        if (d->prim == ESPGPU_PRIM_LINE_LOOP && count >= 2) { /* GL：两个顶点也闭合 */
            uint32_t a = LOADI(count - 1), b = LOADI(0);
            if (VALID(a) && VALID(b)) draw_line(&dc, a, b);
        }
        break;
    case ESPGPU_PRIM_POINTS:
        for (uint32_t k = 0; k < count; k++) {
            uint32_t a = LOADI(k);
            if (VALID(a)) draw_point(&dc, a);
        }
        break;
    default:
        break;
    }
#undef VALID
#undef LOADI
}
