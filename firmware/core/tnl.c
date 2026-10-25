/*
 * tnl.c — 顶点取数、变换与光照、裁剪、图元装配、三角形建立与分箱
 *
 * 流程：DRAW → 按索引取顶点（64 项后变换缓存）→ MVP / 光照 / 纹理矩阵 / 雾
 *       → 齐次裁剪（近/远 + 带保护带的 x/y）→ 透视除法 + 视口
 *       → 剔除 → 边函数 + 属性平面 → 按包围盒分进 tile。
 *
 * 浮点：S3 有单精度 FPU，每帧几千个顶点，全用 float 最省事也最准。
 */

#include <math.h>

#include "core_internal.h"


/* ── 属性解码 ─────────────────────────────────────────────────────── */

static inline float rd_f32(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static void fetch_attr(const uint8_t *base, const espgpu_vertex_attrib_t *a, float out[4])
{
    const uint8_t *p = base + a->offset;
    const int n = a->components < 1 ? 1 : (a->components > 4 ? 4 : a->components);
    const float scale = a->frac_bits ? 1.0f / (float)(1u << (a->frac_bits & 31)) : 1.0f;

    switch (a->type) {
    case ESPGPU_TYPE_F32:
        for (int i = 0; i < n; i++) out[i] = rd_f32(p + i * 4);
        break;
    case ESPGPU_TYPE_I8:
        for (int i = 0; i < n; i++) {
            const int8_t v = (int8_t)p[i];
            out[i] = a->normalized ? (v < -127 ? -1.0f : (float)v / 127.0f) : (float)v * scale;
        }
        break;
    case ESPGPU_TYPE_U8:
        for (int i = 0; i < n; i++) {
            out[i] = a->normalized ? (float)p[i] / 255.0f : (float)p[i] * scale;
        }
        break;
    case ESPGPU_TYPE_I16:
        for (int i = 0; i < n; i++) {
            int16_t v;
            memcpy(&v, p + i * 2, 2);
            out[i] = a->normalized ? (v < -32767 ? -1.0f : (float)v / 32767.0f) : (float)v * scale;
        }
        break;
    case ESPGPU_TYPE_U16:
        for (int i = 0; i < n; i++) {
            uint16_t v;
            memcpy(&v, p + i * 2, 2);
            out[i] = a->normalized ? (float)v / 65535.0f : (float)v * scale;
        }
        break;
    case ESPGPU_TYPE_I32:
        for (int i = 0; i < n; i++) {
            int32_t v;
            memcpy(&v, p + i * 4, 4);
            out[i] = a->normalized ? (float)v / 2147483647.0f : (float)v * scale;
        }
        break;
    case ESPGPU_TYPE_U32:
        for (int i = 0; i < n; i++) {
            uint32_t v;
            memcpy(&v, p + i * 4, 4);
            out[i] = a->normalized ? (float)v / 4294967295.0f : (float)v * scale;
        }
        break;
    case ESPGPU_TYPE_FIXED16_16:
        for (int i = 0; i < n; i++) {
            int32_t v;
            memcpy(&v, p + i * 4, 4);
            out[i] = (float)v / 65536.0f;
        }
        break;
    case ESPGPU_TYPE_RGB565: {
        uint16_t v;
        memcpy(&v, p, 2);
        out[0] = (float)((v >> 11) & 0x1F) / 31.0f;
        out[1] = (float)((v >> 5) & 0x3F) / 63.0f;
        out[2] = (float)(v & 0x1F) / 31.0f;
        out[3] = 1.0f;
        break;
    }
    case ESPGPU_TYPE_RGBA4444: {
        uint16_t v;
        memcpy(&v, p, 2);
        out[0] = (float)((v >> 12) & 0xF) / 15.0f;
        out[1] = (float)((v >> 8) & 0xF) / 15.0f;
        out[2] = (float)((v >> 4) & 0xF) / 15.0f;
        out[3] = (float)(v & 0xF) / 15.0f;
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
    gp_olayout_t ol;
    bool lighting;
    bool fog;
    bool vm;
    uint8_t varyings;
    bool flat;
    bool cull;
    float gx, gy;       /* 保护带系数 */
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

static float fog_factor(const gpu_core_t *c, float dist)
{
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
        const float span = c->fog.fog_end - c->fog.fog_start;
        f = span != 0.0f ? (c->fog.fog_end - dist) / span : 1.0f;
        break;
    }
    }
    return gp_clampf(f, 0.0f, 1.0f);
}

/** 处理一个顶点到裁剪空间 */
static void process_vertex(draw_ctx_t *dc, uint32_t index, gp_cvtx_t *o)
{
    gpu_core_t *c = dc->c;
    const uint8_t *base = dc->vdata + (size_t)index * dc->stride;

    float pos[4] = {0, 0, 0, 1};
    float col[4] = {1, 1, 1, 1};
    float tc[4][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
    float nrm[4] = {0, 0, 1, 0};

    if (dc->attr[ESPGPU_ATTR_POSITION]) {
        fetch_attr(base, dc->attr[ESPGPU_ATTR_POSITION], pos);
    }
    if (dc->attr[ESPGPU_ATTR_COLOR]) {
        /* GL：属性里没给的分量补 (0,0,0,1)，不是 (1,1,1,1) */
        col[0] = col[1] = col[2] = 0.0f;
        fetch_attr(base, dc->attr[ESPGPU_ATTR_COLOR], col);
    } else {
        for (int i = 0; i < 4; i++) col[i] = (float)c->const_color[i] / 255.0f;
    }
    for (int u = 0; u < 4; u++) {
        if (dc->attr[ESPGPU_ATTR_TEXCOORD0 + u]) {
            fetch_attr(base, dc->attr[ESPGPU_ATTR_TEXCOORD0 + u], tc[u]);
        }
    }
    if (dc->attr[ESPGPU_ATTR_NORMAL]) {
        nrm[0] = nrm[1] = nrm[2] = 0.0f;
        nrm[3] = 1.0f;
        fetch_attr(base, dc->attr[ESPGPU_ATTR_NORMAL], nrm);
    }

    mat_vec4(c->mat[ESPGPU_MATRIX_MVP], pos, o->clip);

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
    if (dc->vm) {
        const float *src[6] = {col, tc[0], tc[1], tc[2], tc[3], nrm};
        for (int k = 0; k < dc->varyings; k++) {
            memcpy(&o->a[k * 4], src[k], 16);
        }
    } else {
        if (ol->off_color >= 0) {
            memcpy(&o->a[ol->off_color], col, 16);
        }
        if (ol->off_t0 >= 0) {
            o->a[ol->off_t0] = tc[0][0] * ol->t0_scale[0];
            o->a[ol->off_t0 + 1] = tc[0][1] * ol->t0_scale[1];
        }
        if (ol->off_t1 >= 0) {
            o->a[ol->off_t1] = tc[1][0] * ol->t1_scale[0];
            o->a[ol->off_t1 + 1] = tc[1][1] * ol->t1_scale[1];
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
        o->a[ol->off_fog] = fog_factor(c, fabsf(dist));
    }
}

static const gp_cvtx_t *get_vertex(draw_ctx_t *dc, uint32_t index)
{
    gpu_core_t *c = dc->c;
    const uint32_t slot = index & (GP_VCACHE - 1);
    if (c->vcache[slot].index != index) {
        process_vertex(dc, index, &c->vcache[slot].v);
        c->vcache[slot].index = index;
        c->st_verts++;
    }
    return &c->vcache[slot].v;
}

/* ── 裁剪 ─────────────────────────────────────────────────────────── */

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
    uint32_t m = 0;
    for (int p = 0; p < N_PLANES; p++) {
        if (plane_dist(dc, p, v) < 0.0f) {
            m |= 1u << p;
        }
    }
    return m;
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
static int clip_poly(const draw_ctx_t *dc, uint32_t mask, gp_cvtx_t *buf_a, gp_cvtx_t *buf_b,
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
                res[m++] = *A;
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

/* ── 投影 ─────────────────────────────────────────────────────────── */

static void project(const draw_ctx_t *dc, const gp_cvtx_t *v, gp_wvtx_t *w)
{
    const gpu_core_t *c = dc->c;
    const float iw = 1.0f / v->clip[3];
    const float nx = v->clip[0] * iw;
    const float ny = v->clip[1] * iw;
    const float nz = v->clip[2] * iw;
    w->x = (float)c->vp_x + (nx + 1.0f) * 0.5f * (float)c->vp_w;
    w->y = (float)c->vp_y + (1.0f - ny) * 0.5f * (float)c->vp_h;
    float z = c->depth_n + (nz + 1.0f) * 0.5f * (c->depth_f - c->depth_n);
    z = z * 65535.0f - (float)c->ext.depth_bias;
    w->z = gp_clampf(z, 0.0f, 65535.0f);
    w->iw = iw;
    memcpy(w->a, v->a, sizeof(float) * dc->ol.n);
}

/* ── 三角形建立 ───────────────────────────────────────────────────── */

static inline int32_t floor_div16(int32_t v)
{
    return v >= 0 ? v / 16 : -((-v + 15) / 16);
}

static void make_plane(gp_plane_t *p, float x0, float y0, float x1, float y1, float x2, float y2,
                       float inv_det, float v0, float v1, float v2)
{
    const float d1 = v1 - v0;
    const float d2 = v2 - v0;
    p->dx = (d1 * (y2 - y0) - d2 * (y1 - y0)) * inv_det;
    p->dy = (d2 * (x1 - x0) - d1 * (x2 - x0)) * inv_det;
    p->c = v0 + p->dx * (0.5f - x0) + p->dy * (0.5f - y0);
}

bool gp_setup_tri(gpu_core_t *c, const gp_wvtx_t *v0, const gp_wvtx_t *v1, const gp_wvtx_t *v2,
                  const gp_olayout_t *ol, uint32_t state_off_unused, bool cull_enabled)
{
    (void)state_off_unused;
    const bool is2d = (ol->flags & GP_TF_2D) != 0;

    const gp_wvtx_t *V[3] = {v0, v1, v2};
    int32_t X[3], Y[3];
    for (int i = 0; i < 3; i++) {
        /* NaN / 极端坐标（坏矩阵、坏视口）：直接丢弃。±2^20 像素内边函数系数放得进 int32 */
        if (!(fabsf(V[i]->x) < 1048576.0f) || !(fabsf(V[i]->y) < 1048576.0f)) {
            return true;
        }
        X[i] = (int32_t)lrintf(V[i]->x * 16.0f);
        Y[i] = (int32_t)lrintf(V[i]->y * 16.0f);
    }
    int64_t area = (int64_t)(X[1] - X[0]) * (Y[2] - Y[0]) - (int64_t)(Y[1] - Y[0]) * (X[2] - X[0]);
    if (area == 0) {
        return true;
    }

    if (cull_enabled && c->ps.cull_mode != ESPGPU_CULL_NONE) {
        /* 窗口坐标 y 向下：面积为正 = NDC 里顺时针 */
        const bool front = (c->ps.front_face == ESPGPU_FRONT_CCW) ? (area < 0) : (area > 0);
        const uint8_t cm = c->ps.cull_mode;
        if (cm == ESPGPU_CULL_BOTH || (cm == ESPGPU_CULL_BACK && !front) ||
            (cm == ESPGPU_CULL_FRONT && front)) {
            c->st_culled++;
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

    /* 包围盒（像素中心 16p+8 落在 [min,max] 内） */
    int32_t minx = X[0], maxx = X[0], miny = Y[0], maxy = Y[0];
    for (int i = 1; i < 3; i++) {
        if (X[i] < minx) minx = X[i];
        if (X[i] > maxx) maxx = X[i];
        if (Y[i] < miny) miny = Y[i];
        if (Y[i] > maxy) maxy = Y[i];
    }
    int bx0 = floor_div16(minx - 8 + 15);
    int by0 = floor_div16(miny - 8 + 15);
    int bx1 = floor_div16(maxx - 8);
    int by1 = floor_div16(maxy - 8);
    if (bx0 < c->sc_x) bx0 = c->sc_x;
    if (by0 < c->sc_y) by0 = c->sc_y;
    if (bx1 > c->sc_x + c->sc_w - 1) bx1 = c->sc_x + c->sc_w - 1;
    if (by1 > c->sc_y + c->sc_h - 1) by1 = c->sc_y + c->sc_h - 1;
    if (bx0 > bx1 || by0 > by1) {
        return true;
    }

    const uint32_t n_att = ol->n;
    const uint32_t rec_bytes = (uint32_t)(sizeof(gp_tri_t) + n_att * sizeof(gp_plane_t));
    const uint32_t n_tiles = (uint32_t)(((bx1 >> GP_TILE_SHIFT) - (bx0 >> GP_TILE_SHIFT) + 1) *
                                        ((by1 >> GP_TILE_SHIFT) - (by0 >> GP_TILE_SHIFT) + 1));
    if (!gp_reserve(c, rec_bytes, n_tiles)) {
        return false;
    }
    const uint32_t state_off = gp_snapshot_state(c, is2d);
    if (state_off == 0) {
        return false;
    }

    uint32_t off;
    gp_tri_t *t = (gp_tri_t *)gp_arena_alloc(c, rec_bytes, &off);
    if (t == NULL) {
        return false;
    }
    t->type = GP_REC_TRI;
    t->n_att = (uint8_t)n_att;
    t->pad = 0;
    t->flags = (uint8_t)(ol->flags & ~GP_TF_2D);
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
    for (int k = 0; k < 3; k++) {
        const int i = (k + 1) % 3;
        const int j = (k + 2) % 3;
        const int64_t dx = (int64_t)X[j] - X[i];
        const int64_t dy = (int64_t)Y[j] - Y[i];
        const int64_t a = -16 * dy;
        const int64_t b = 16 * dx;
        int64_t cc = dx * (8 - (int64_t)Y[i]) - dy * (8 - (int64_t)X[i]);
        const bool top_left = (a > 0) || (a == 0 && b > 0);
        if (!top_left) {
            cc -= 1;
        }
        t->ea[k] = (int32_t)a;
        t->eb[k] = (int32_t)b;
        t->ec[k] = cc;
        t->inv_ea[k] = a != 0 ? 1.0f / (float)a : 0.0f;
    }

    /* 平面 */
    const float x0 = (float)X[0] / 16.0f, y0 = (float)Y[0] / 16.0f;
    const float x1 = (float)X[1] / 16.0f, y1 = (float)Y[1] / 16.0f;
    const float x2 = (float)X[2] / 16.0f, y2 = (float)Y[2] / 16.0f;
    const float inv_det = 256.0f / (float)area;

    make_plane(&t->z, x0, y0, x1, y1, x2, y2, inv_det, V[0]->z, V[1]->z, V[2]->z);

    const float w0 = V[0]->iw, w1 = V[1]->iw, w2 = V[2]->iw;
    const float tol = fabsf(w0) * 1e-5f;
    const bool affine = fabsf(w1 - w0) <= tol && fabsf(w2 - w0) <= tol;
    if (affine) {
        t->flags |= GP_TF_AFFINE;
        t->iw.c = w0;
        t->iw.dx = t->iw.dy = 0.0f;
        for (uint32_t k = 0; k < n_att; k++) {
            make_plane(&t->att[k], x0, y0, x1, y1, x2, y2, inv_det, V[0]->a[k], V[1]->a[k],
                       V[2]->a[k]);
        }
    } else {
        make_plane(&t->iw, x0, y0, x1, y1, x2, y2, inv_det, w0, w1, w2);
        for (uint32_t k = 0; k < n_att; k++) {
            make_plane(&t->att[k], x0, y0, x1, y1, x2, y2, inv_det, V[0]->a[k] * w0,
                       V[1]->a[k] * w1, V[2]->a[k] * w2);
        }
    }

    gp_bin_record(c, off, bx0, by0, bx1, by1);
    c->st_tris++;
    return true;
}

/** 失败（arena 满）时刷新后重试一次 */
static void emit_tri(draw_ctx_t *dc, const gp_wvtx_t *a, const gp_wvtx_t *b, const gp_wvtx_t *c,
                     bool cull)
{
    if (!gp_setup_tri(dc->c, a, b, c, &dc->ol, 0, cull)) {
        gp_flush(dc->c);
        if (!gp_setup_tri(dc->c, a, b, c, &dc->ol, 0, cull)) {
            gp_log(GP_LOG_WARN, "triangle dropped: arena too small");
        }
    }
}

/* ── 图元 ─────────────────────────────────────────────────────────── */

static void apply_flat(draw_ctx_t *dc, gp_cvtx_t *v, int n, const gp_cvtx_t *provoking)
{
    if (!dc->flat || dc->ol.off_color < 0) {
        return;
    }
    for (int i = 0; i < n; i++) {
        memcpy(&v[i].a[dc->ol.off_color], &provoking->a[dc->ol.off_color], 16);
    }
}

/** prov = 激发顶点的索引（平面着色时取它的颜色） */
static void draw_triangle(draw_ctx_t *dc, uint32_t i0, uint32_t i1, uint32_t i2, uint32_t prov)
{
    gp_cvtx_t va[3 + N_PLANES + 2];
    gp_cvtx_t vb[3 + N_PLANES + 2];

    if (dc->flat) {
        /* 先取激发顶点：它和 i0..i2 可能落在同一个缓存槽 */
        const gp_cvtx_t p = *get_vertex(dc, prov);
        va[0] = *get_vertex(dc, i0);
        va[1] = *get_vertex(dc, i1);
        va[2] = *get_vertex(dc, i2);
        apply_flat(dc, va, 3, &p);
    } else {
        va[0] = *get_vertex(dc, i0);
        va[1] = *get_vertex(dc, i1);
        va[2] = *get_vertex(dc, i2);
    }

    const uint32_t o0 = outcode(dc, va[0].clip);
    const uint32_t o1 = outcode(dc, va[1].clip);
    const uint32_t o2 = outcode(dc, va[2].clip);
    if (o0 & o1 & o2) {
        dc->c->st_culled++;
        return;
    }
    dc->c->st_prims++;

    gp_cvtx_t *poly = va;
    int n = 3;
    const uint32_t any = o0 | o1 | o2;
    if (any) {
        n = clip_poly(dc, any, va, vb, 3, &poly);
        if (n < 3) {
            return;
        }
    }

    gp_wvtx_t w[3 + N_PLANES + 2];
    for (int i = 0; i < n; i++) {
        project(dc, &poly[i], &w[i]);
    }
    /* 扇形三角化；裁剪不改变绕序，剔除判断仍然有效 */
    for (int i = 1; i + 1 < n; i++) {
        emit_tri(dc, &w[0], &w[i], &w[i + 1], true);
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
    emit_tri(dc, &q[0], &q[1], &q[2], false);
    emit_tri(dc, &q[0], &q[2], &q[3], false);
}

static void draw_line(draw_ctx_t *dc, uint32_t i0, uint32_t i1)
{
    gp_cvtx_t a = *get_vertex(dc, i0);
    gp_cvtx_t b = *get_vertex(dc, i1);
    if (dc->flat) {
        apply_flat(dc, &a, 1, &b);
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
    /* 法向偏移半个线宽，两端各延长半个像素，让端点像素被覆盖 */
    emit_quad(dc, &wa, &wb, -dy * hw, dx * hw, dx * 0.5f, dy * 0.5f);
    dc->c->st_prims++;
}

static void draw_point(draw_ctx_t *dc, uint32_t i0)
{
    const gp_cvtx_t *v = get_vertex(dc, i0);
    if (outcode(dc, v->clip)) {
        return;
    }
    gp_wvtx_t w;
    project(dc, v, &w);
    const float hs = (dc->c->ext.point_size ? (float)dc->c->ext.point_size : 1.0f) * 0.5f;
    emit_quad(dc, &w, &w, 0.0f, hs, hs, 0.0f);
    dc->c->st_prims++;
}

/* ── DRAW ─────────────────────────────────────────────────────────── */

void gp_draw3d(gpu_core_t *c, const espgpu_draw_t *d, const uint8_t *inline_data,
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
        if (d->prim == ESPGPU_PRIM_LINE_LOOP && count > 2) {
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
