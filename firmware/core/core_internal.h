/*
 * core_internal.h — 设备核心内部结构
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core.h"
#include "espgpu_proto.h"
#include "espgpu_vm.h"
#include "port.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 强制内联：光栅内核按状态组合实例化，靠常量参数折叠掉分支 */
#if defined(_MSC_VER) && !defined(__clang__)
#define GP_INLINE static __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define GP_INLINE static inline __attribute__((always_inline))
#else
#define GP_INLINE static inline
#endif

/** 禁止内联：冷路径（越界折回、混合）不要被复制进每个光栅内核，IRAM 有限 */
#if defined(_MSC_VER) && !defined(__clang__)
#define GP_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define GP_NOINLINE __attribute__((noinline))
#else
#define GP_NOINLINE
#endif

/* ── 尺寸常量 ─────────────────────────────────────────────────────── */

#define GP_TILE_SHIFT 6
#define GP_TILE (1 << GP_TILE_SHIFT) /* 64x64：RGB565 8 KB + 深度 8 KB */
#define GP_MAX_W 800
#define GP_MAX_H 600
#define GP_MAX_TILES_X ((GP_MAX_W + GP_TILE - 1) / GP_TILE)
#define GP_MAX_TILES_Y ((GP_MAX_H + GP_TILE - 1) / GP_TILE)
#define GP_MAX_TILES (GP_MAX_TILES_X * GP_MAX_TILES_Y)

/** 每顶点插值标量上限：VM 模式 6 个 vec4 + 1 */
#define GP_MAX_ATT 28
#define GP_VCACHE 64
#define GP_MAX_TARGETS 8
#define GP_GUARD_PX 1024.0f
#define GP_BIN_CHUNK 30

/* ── 纹理 ─────────────────────────────────────────────────────────── */

typedef struct gp_tex {
    uint32_t id;
    uint16_t w, h;
    uint8_t fmt;        /* 存储格式：RGB565 / INDEX8 / R8 */
    uint8_t priority;   /* ESPGPU_TEXF_PRIORITY */
    uint8_t has_alpha;  /* alpha 平面或调色板 alpha 非全 255 */
    uint8_t is_target;  /* 当前被当作 FBO 目标 */
    uint16_t *px;       /* RGB565，fmt == RGB565 */
    uint8_t *idx;       /* INDEX8 / R8 */
    uint8_t *alpha;     /* RGBA4444 展开的 alpha 平面，可空 */
    uint16_t *pal;      /* INDEX8：256 × RGB565 */
    uint8_t *pal_a;     /* INDEX8：256 × alpha，可空 */
    uint32_t bytes;
    uint32_t last_used; /* LRU 时间戳（帧号） */
    uint32_t bin_epoch; /* 被当前未刷新的分箱引用时 == core->bin_epoch */
    uint32_t received;
    uint32_t total_len;
} gp_tex_t;

typedef struct {
    gp_tex_t **slots;   /* 开放寻址哈希，NULL = 空，GP_TEX_TOMB = 删除 */
    uint32_t cap;
    uint32_t count;
    uint32_t tombs;     /* GP_TEX_TOMB 个数 */
    uint32_t bytes;
    uint32_t budget;
    uint32_t evictions;
    uint32_t misses;
} gp_texcache_t;

/* ── 设备端缓冲 ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t id;
    uint32_t len;
    uint8_t *data;
} gp_buffer_t;

/* ── 片元程序 ─────────────────────────────────────────────────────── */

typedef struct {
    uint16_t count;
    uint8_t varyings;
    uint8_t valid;
    uint8_t ntemps;     /* 用到的临时寄存器个数：每像素只清这么多 */
    espgpu_vm_insn_t code[ESPGPU_VM_MAX_INSNS];
} gp_prog_t;

/* ── 渲染目标 ─────────────────────────────────────────────────────── */

typedef struct {
    uint16_t w, h;
    uint16_t *color;
    uint16_t *depth;     /* 懒分配 */
    uint8_t color_fast;  /* color 在内部 SRAM：直接光栅，不走 tile 拷贝 */
    uint8_t bottom_up;   /* FBO 纹理目标：视口 / 裁剪按左下角原点给出 */
    uint32_t tex_id;     /* 0 = 屏幕 */
} gp_target_t;

/* ── 光栅状态快照（分箱时冻结，存在 arena 里） ───────────────────── */

typedef struct {
    const gp_tex_t *tex;
    uint8_t filter;
    uint8_t wrap_u;
    uint8_t wrap_v;
    uint8_t pad;
} gp_sampler_t;

typedef struct {
    uint8_t shade;
    uint8_t blend;      /* 是否混合 */
    uint8_t bsrc, bdst, bop;
    uint8_t depth_test, depth_write, depth_func;
    uint8_t alpha_test, alpha_func, alpha_ref;
    uint8_t fog;
    uint8_t color_write;
    uint8_t vm;         /* PROGRAM 模式 */
    uint8_t fog_r, fog_g, fog_b;
    uint8_t pad;
    gp_sampler_t smp[ESPGPU_MAX_TEXTURE_UNITS];
    const gp_prog_t *prog;
    const float *consts; /* ESPGPU_VM_CONSTS × 4，快照 */
} gp_rstate_t;

/* ── 分箱记录 ─────────────────────────────────────────────────────── */

enum {
    GP_REC_TRI = 1,
    GP_REC_CLEAR = 2,
};

enum {
    GP_TF_AFFINE = 1u << 0,      /* 三个顶点 1/w 相同：属性直接线性插值 */
    GP_TF_CONST_COLOR = 1u << 1, /* 颜色不插值，取 const_rgba */
    GP_TF_TEX0 = 1u << 2,        /* 有 u0 v0（texel 单位） */
    GP_TF_TEX1 = 1u << 3,
    GP_TF_FOG = 1u << 4,
    GP_TF_VM = 1u << 5,          /* 属性是 VM varying（归一化纹理坐标） */
    GP_TF_UNIT = 1u << 6,        /* 颜色属性三个顶点都在 [0,1]（雾因子总在 [0,1]）：三角形内插值
                                  * 不会越界，像素管线快速路径只在段端点夹取（见 raster3d.c） */
    GP_TF_2D = 1u << 7,          /* 仅用于 olayout：用 2D 管线状态快照 */
};

/** 平面方程 p(x,y) = c + dx*x + dy*y，(x,y) 为整数像素坐标（中心已折进 c） */
typedef struct {
    float c, dx, dy;
} gp_plane_t;

typedef struct {
    uint8_t type;   /* GP_REC_TRI */
    uint8_t flags;  /* GP_TF_* */
    uint8_t n_att;
    uint8_t pad;
    int8_t o_col, o_t0, o_t1, o_fog; /* 属性偏移，-1 = 无 */
    int16_t bx0, by0, bx1, by1; /* 像素包围盒（含），已与裁剪矩形 / 目标求交 */
    uint32_t state_off;
    /* 边函数：E(px,py) = a*px + b*py + c，px/py 为整数像素，>= 0 在内（已含 top-left 偏置） */
    int64_t ec[3];
    int32_t ea[3];
    int32_t eb[3];
    float inv_ea[3];  /* 1/ea，用于跨度估算 */
    uint8_t const_rgba[4];
    gp_plane_t z;     /* 0..65535 */
    gp_plane_t iw;    /* 1/w */
    gp_plane_t att[]; /* n_att 个 */
} gp_tri_t;

typedef struct {
    uint8_t type;   /* GP_REC_CLEAR */
    uint8_t flags;  /* bit0 颜色 bit1 深度 */
    uint16_t color;
    uint16_t depth;
    uint16_t pad;
    int16_t x0, y0, x1, y1; /* 含 */
} gp_clear_t;

typedef struct gp_bin_chunk {
    uint32_t next;  /* arena 偏移，0 = 无 */
    uint16_t count;
    uint16_t pad;
    uint32_t rec[GP_BIN_CHUNK];
} gp_bin_chunk_t;

typedef struct {
    uint32_t head;  /* arena 偏移 */
    uint32_t tail;
    uint32_t count;
} gp_bin_t;

/* ── 顶点 ─────────────────────────────────────────────────────────── */

/** 裁剪空间顶点 */
typedef struct {
    float clip[4];
    float a[GP_MAX_ATT];
} gp_cvtx_t;

/** 窗口空间顶点 */
typedef struct {
    float x, y, z, iw;
    float a[GP_MAX_ATT];
} gp_wvtx_t;

/**
 * 后变换缓存项：裁剪坐标 + outcode + 已投影的窗口坐标（w > 0 时有效）+ 属性（只存一份，
 * 窗口空间与裁剪空间的属性相同）。不需要裁剪的三角形直接拿缓存项建立，不再逐三角形拷贝 / 投影。
 */
typedef struct {
    uint32_t index;
    uint32_t oc;        /* 裁剪 outcode（tnl.c 的 N_PLANES 位） */
    float clip[4];
    gp_wvtx_t w;
} gp_vce_t;

/** 本次绘制的属性输出布局 */
typedef struct {
    uint8_t n;
    uint8_t flags;      /* GP_TF_TEX0 / TEX1 / FOG / VM / CONST_COLOR */
    int8_t off_color;   /* -1 = 无 */
    int8_t off_t0, off_t1, off_fog;
    float t0_scale[2];  /* 归一化 → texel */
    float t1_scale[2];
} gp_olayout_t;

/* ── 每 worker 的 tile 工作区 ─────────────────────────────────────── */

typedef struct {
    uint16_t *color;    /* GP_TILE*GP_TILE，屏幕目标在快速内存时不用 */
    uint16_t *depth;
    float vm_regs[ESPGPU_VM_TEMPS * 4];
    uint32_t fragments;
    /* 本次 gp_raster_frame 的分段计数（gp_cycles 单位，worker 自己的核上做差） */
    uint32_t cy_raster;  /* tile 内光栅（不含 tile 取 / 写回） */
    uint32_t cy_io;      /* tile 颜色 / 深度取入与写回 */
    uint32_t n_path[4];  /* 走 fill / fast / px / generic 的三角形×tile 数 */
} gp_worker_t;

enum { GP_PATH_FILL = 0, GP_PATH_FAST, GP_PATH_PX, GP_PATH_GENERIC };

/* ── 核心 ─────────────────────────────────────────────────────────── */

struct gpu_core {
    gpu_core_cb_t cb;
    gpu_core_config_t cfg;

    /* 渲染目标 */
    gp_target_t screen;
    gp_target_t fbo[GP_MAX_TARGETS];
    gp_target_t *target;

    /* 视口 / 裁剪（目标内部的左上角坐标） */
    int16_t vp_x, vp_y, vp_w, vp_h;
    int16_t sc_x, sc_y, sc_w, sc_h;
    espgpu_rect_t vp_req, sc_req; /* 主机给的原值，换目标时重算 */
    bool sc_req_full;
    bool vp_req_full;
    float depth_n, depth_f;

    /* 3D 管线状态 */
    espgpu_pipeline_t ps;
    espgpu_pipeline_fog_t fog;
    espgpu_pipeline_ext_t ext;
    espgpu_vertex_layout_t layout;
    bool layout_valid;
    float mat[ESPGPU_MATRIX_COUNT][16];
    bool mat_set[ESPGPU_MATRIX_COUNT];
    uint8_t const_color[4];
    espgpu_lighting_t light_hdr;
    espgpu_light_t lights[ESPGPU_MAX_LIGHTS];

    /* 纹理绑定（id），3D 与 2D 共用 */
    uint32_t bind_id[ESPGPU_MAX_TEXTURE_UNITS];
    uint8_t bind_filter[ESPGPU_MAX_TEXTURE_UNITS];
    uint8_t bind_wrap_u[ESPGPU_MAX_TEXTURE_UNITS];
    uint8_t bind_wrap_v[ESPGPU_MAX_TEXTURE_UNITS];

    /* M2 的 2D 管线 */
    espgpu_pipeline2d_t p2d;

    /* VM */
    gp_prog_t *progs[ESPGPU_VM_MAX_PROGRAMS];
    float consts[ESPGPU_VM_CONSTS * 4];

    /* 纹理 / 缓冲 */
    gp_texcache_t tc;
    gp_buffer_t *buffers;   /* ESPGPU_MAX_BUFFERS */
    uint32_t buffer_bytes;

    /* 分箱 */
    uint8_t *arena;
    uint32_t arena_cap;
    uint32_t arena_used;
    gp_bin_t bins[GP_MAX_TILES];
    int tiles_x, tiles_y;
    uint32_t bin_epoch;
    uint32_t rstate_off;    /* 当前状态快照，0 = 需要重新快照 */
    uint32_t rstate_2d_off;
    uint32_t consts_off;    /* 当前常量快照，0 = 需要重新快照 */
    bool pending;           /* 有未刷新的记录 */

    /* 后变换缓存 */
    gp_vce_t *vcache;

    /* 工作区 */
    gp_worker_t workers[GP_MAX_WORKERS];
    int nworkers;
    volatile int next_tile;

    /* 调试 */
    uint8_t debug_view;
    uint8_t *dbg;           /* 每像素 overdraw / 线框标记，屏幕大小 */
    uint16_t *dbg_img;      /* 合成后的调试画面 */
    uint32_t dbg_tile_tris[GP_MAX_TILES]; /* 本帧每 tile 光栅的三角形数（分箱在 PRESENT 前就清了） */

    /* 统计 */
    uint32_t frame;
    uint32_t st_tris, st_culled, st_bin_peak, st_tnl_us, st_draws, st_prims, st_verts;
    uint32_t last_tris, last_culled, last_bin, last_tnl_us;
    uint32_t last_fill;
    uint32_t st_raster_us, last_raster_us, last_fill_frame;

    /* 分段计时（gp_cycles 单位，按帧累加；PRESENT 时换算成 µs 存进 last_*） */
    uint64_t cy_vtx, cy_clip, cy_setup, cy_bin, cy_raster, cy_io;
    uint32_t st_path[4];
    uint32_t last_vtx_us, last_clip_us, last_setup_us, last_bin_us, last_tile_us, last_io_us;
    uint32_t last_path[4];
    uint32_t last_flushes;
    uint32_t st_flushes;
};

/* ── 跨文件接口 ───────────────────────────────────────────────────── */

/* core.c */
void *gp_arena_alloc(gpu_core_t *c, uint32_t bytes, uint32_t *off_out);
static inline void *gp_arena_ptr(gpu_core_t *c, uint32_t off)
{
    return c->arena + off;
}
void gp_flush(gpu_core_t *c);
uint32_t gp_snapshot_state(gpu_core_t *c, bool for_2d);
void gp_bin_record(gpu_core_t *c, uint32_t rec_off, int x0, int y0, int x1, int y1);
/** 把记录追加到一个 tile 的链表（不置 pending，调用者负责） */
void gp_bin_tile(gpu_core_t *c, int tile, uint32_t rec_off);
void gp_bin_clear(gpu_core_t *c, uint8_t flags, uint16_t color, uint16_t depth);
bool gp_target_need_depth(gpu_core_t *c, gp_target_t *t);
void gp_send_event(gpu_core_t *c, uint8_t kind, uint16_t value, uint32_t arg);
/** 预留 arena 空间，不够就先刷新；仍不够返回 false */
bool gp_reserve(gpu_core_t *c, uint32_t rec_bytes, uint32_t n_tiles);
const uint8_t *gp_buffer_data(gpu_core_t *c, uint32_t id, uint32_t *len);

/* tnl.c */
void gp_draw3d(gpu_core_t *c, const espgpu_draw_t *d, const uint8_t *inline_data,
               uint32_t inline_len);
/**
 * 窗口空间三角形 → 剔除 / 建立 / 分箱。返回 false 表示 arena 满（调用者应刷新后重试）。
 * flat_col：平面着色时激发顶点的颜色（ol->off_color 处的 4 个分量），NULL = 正常插值。
 */
bool gp_setup_tri(gpu_core_t *c, const gp_wvtx_t *v0, const gp_wvtx_t *v1, const gp_wvtx_t *v2,
                  const gp_olayout_t *ol, const float *flat_col, bool cull_enabled);

/* raster2d.c */
void gp_draw2d(gpu_core_t *c, const espgpu_draw_t *d, const uint8_t *verts, uint32_t len);

/* raster3d.c */
void gp_raster_frame(gpu_core_t *c);
void gp_compose_debug(gpu_core_t *c);

/* tex.c */
bool gp_tc_init(gp_texcache_t *tc, uint32_t budget);
void gp_tc_destroy(gp_texcache_t *tc);
gp_tex_t *gp_tex_find(gp_texcache_t *tc, uint32_t id);
gp_tex_t *gp_tex_get(gpu_core_t *c, uint32_t id);
void gp_tex_delete(gpu_core_t *c, uint32_t id);
void gp_tex_delete_all(gpu_core_t *c);
bool gp_tex_upload(gpu_core_t *c, const espgpu_upload_texture_t *u, uint8_t flags,
                   const uint8_t *data, uint32_t len);
void gp_tex_palette(gpu_core_t *c, const espgpu_tex_palette_t *p, const uint8_t *data,
                    uint32_t len);
/** 为 FBO / 拷贝准备一张 w×h 的 RGB565 纹理 */
gp_tex_t *gp_tex_make_rgb565(gpu_core_t *c, uint32_t id, uint16_t w, uint16_t h);
/** 纹理即将被修改：若被未刷新的分箱引用，先刷新 */
void gp_tex_before_modify(gpu_core_t *c, gp_tex_t *t);

/* 采样：返回 0xAARRGGBB；u,v 为 texel 坐标 */
uint32_t gp_sample(const gp_sampler_t *s, float u, float v);

/* vm.c */
/** 对一个片元执行程序。v 为 ESPGPU_VM_VARYINGS*4 的输入，out 为 rgba [0,1]。返回 false = 丢弃 */
bool gp_vm_run(const gp_prog_t *p, const float *consts, const gp_sampler_t *smp, const float *v,
               float *regs, float out[4]);
bool gp_vm_validate(const espgpu_vm_insn_t *code, uint32_t n);
/** 程序内存要释放了：丢掉 vm.c 缓存的解码结果（按 code 指针索引）。释放 gp_prog_t 前调用 */
void gp_vm_forget(const espgpu_vm_insn_t *code);
/**
 * 一段 n（1..8）个片元：第 i 个的 varying = v0 累加 i 次 dv（布局同 gp_vm_run 的 v）。
 * out[i] 为第 i 个的颜色；返回未被 KIL 的片元位掩码（bit i）。结果与逐个调 gp_vm_run 逐位相同。
 */
uint32_t gp_vm_run_span(const gp_prog_t *p, const float *consts, const gp_sampler_t *smp,
                        const float *v0, const float *dv, int n, float *regs, float (*out)[4]);
/** 程序引用到的最大临时寄存器编号 + 1（validate 通过后调用） */
uint8_t gp_vm_temps_used(const espgpu_vm_insn_t *code, uint32_t n);

/* ── 小工具 ───────────────────────────────────────────────────────── */

static inline uint32_t gp_expand565(uint16_t c)
{
    const uint32_t r = (c >> 11) & 0x1F;
    const uint32_t g = (c >> 5) & 0x3F;
    const uint32_t b = c & 0x1F;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) |
           ((b << 3) | (b >> 2));
}

static inline uint16_t gp_pack565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline int gp_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float gp_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/** a*b/255，精确到 ±1 */
static inline uint32_t gp_mul8(uint32_t a, uint32_t b)
{
    const uint32_t t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

/**
 * 比较函数的位掩码形式：ESPGPU_CMP_* 恰好是 GL 的 {小于=1, 等于=2, 大于=4} 组合，
 * 于是 pass = (mask >> ((a >= b) + (a > b))) & 1，内层循环里不用 switch。
 */
static inline uint32_t gp_cmp_mask(uint8_t func)
{
    return func > ESPGPU_CMP_ALWAYS ? 7u : func;
}

static inline uint32_t gp_cmp_m(uint32_t mask, uint32_t a, uint32_t b)
{
    return (mask >> ((uint32_t)(a >= b) + (uint32_t)(a > b))) & 1u;
}

/** floor 后转 int；非有限或超出 ±2^30 时按 0 处理（避免 float→int 越界 UB），范围内与 (int)floorf 相同 */
static inline int gp_floor_i(float f)
{
    if (!(f > -1073741824.0f && f < 1073741824.0f)) {
        return 0;
    }
    const int i = (int)f;
    return (f < (float)i) ? i - 1 : i;
}

static inline bool gp_cmp(uint8_t func, uint32_t a, uint32_t b)
{
    switch (func) {
    case ESPGPU_CMP_NEVER: return false;
    case ESPGPU_CMP_LESS: return a < b;
    case ESPGPU_CMP_EQUAL: return a == b;
    case ESPGPU_CMP_LEQUAL: return a <= b;
    case ESPGPU_CMP_GREATER: return a > b;
    case ESPGPU_CMP_NOTEQUAL: return a != b;
    case ESPGPU_CMP_GEQUAL: return a >= b;
    default: return true;
    }
}

/* ── 纹理采样（内联版本：光栅器直接展开，tex.c 的 gp_sample 也调用它）。
 *    GP_HOT：编译器若没内联，留下的副本也要在 IRAM 里 ── */

GP_HOT static inline int gp_wrap(int v, int n, uint8_t mode)
{
    if (v >= 0 && v < n) {
        return v;
    }
    switch (mode) {
    case ESPGPU_WRAP_CLAMP:
        return v < 0 ? 0 : n - 1;
    case ESPGPU_WRAP_MIRROR: {
        int m = v % (2 * n);
        if (m < 0) m += 2 * n;
        return m < n ? m : 2 * n - 1 - m;
    }
    default: {
        int m = v % n;
        return m < 0 ? m + n : m;
    }
    }
}

GP_HOT static inline uint32_t gp_texel(const gp_tex_t *t, int x, int y)
{
    const size_t i = (size_t)y * t->w + x;
    if (t->px) {
        uint32_t c = gp_expand565(t->px[i]);
        if (t->alpha) {
            c = (c & 0x00FFFFFFu) | ((uint32_t)t->alpha[i] << 24);
        }
        return c;
    }
    const uint8_t v = t->idx[i];
    if (t->fmt == ESPGPU_FMT_R8) {
        return 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
    }
    uint32_t c = t->pal ? gp_expand565(t->pal[v])
                        : (0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v);
    if (t->pal_a) {
        c = (c & 0x00FFFFFFu) | ((uint32_t)t->pal_a[v] << 24);
    }
    return c;
}

/** 最近邻；t 必须有像素 */
GP_HOT static inline uint32_t gp_sample_nearest(const gp_sampler_t *s, float u, float v)
{
    const gp_tex_t *t = s->tex;
    const int x = gp_wrap(gp_floor_i(u), t->w, s->wrap_u);
    const int y = gp_wrap(gp_floor_i(v), t->h, s->wrap_v);
    return gp_texel(t, x, y);
}

/** 双线性（8 位权重）；t 必须有像素 */
GP_HOT static inline uint32_t gp_sample_linear(const gp_sampler_t *s, float u, float v)
{
    const gp_tex_t *t = s->tex;
    const int w = t->w, h = t->h;
    const float fu = u - 0.5f;
    const float fv = v - 0.5f;
    const int x0 = gp_floor_i(fu);
    const int y0 = gp_floor_i(fv);
    const float flu = (float)x0;
    const float flv = (float)y0;
    /* 越界 / NaN 坐标 gp_floor_i 给 0，小数部分也要夹住（NaN → 0）才不会把权重算爆 */
    float frx = fu - flu, fry = fv - flv;
    frx = frx > 0.0f ? (frx < 0.99999994f ? frx : 0.99999994f) : 0.0f;
    fry = fry > 0.0f ? (fry < 0.99999994f ? fry : 0.99999994f) : 0.0f;
    const uint32_t ax = (uint32_t)(frx * 256.0f);
    const uint32_t ay = (uint32_t)(fry * 256.0f);
    const int xa = gp_wrap(x0, w, s->wrap_u), xb = gp_wrap(x0 + 1, w, s->wrap_u);
    const int ya = gp_wrap(y0, h, s->wrap_v), yb = gp_wrap(y0 + 1, h, s->wrap_v);
    const uint32_t w00 = (256 - ax) * (256 - ay), w10 = ax * (256 - ay);
    const uint32_t w01 = (256 - ax) * ay, w11 = ax * ay;
    if (t->px && !t->alpha) {
        /* 最常见的不透明 RGB565：直接取像素、alpha 恒 255（四个 255 加权和恰好还是 255），
         * 省掉 gp_texel 的格式分支和一条通道的乘加；结果与通用分支逐位相同 */
        const uint16_t *p = t->px;
        const uint32_t c00 = gp_expand565(p[(size_t)ya * w + xa]), c10 = gp_expand565(p[(size_t)ya * w + xb]);
        const uint32_t c01 = gp_expand565(p[(size_t)yb * w + xa]), c11 = gp_expand565(p[(size_t)yb * w + xb]);
        uint32_t out = 0xFF000000u;
        for (int sh = 0; sh < 24; sh += 8) {
            const uint32_t v4 = ((c00 >> sh) & 255) * w00 + ((c10 >> sh) & 255) * w10 +
                                ((c01 >> sh) & 255) * w01 + ((c11 >> sh) & 255) * w11;
            out |= ((v4 + 32768) >> 16) << sh; /* 权重和 = 65536，不会超过 255 */
        }
        return out;
    }
    const uint32_t c00 = gp_texel(t, xa, ya), c10 = gp_texel(t, xb, ya);
    const uint32_t c01 = gp_texel(t, xa, yb), c11 = gp_texel(t, xb, yb);
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const uint32_t v4 = ((c00 >> sh) & 255) * w00 + ((c10 >> sh) & 255) * w10 +
                            ((c01 >> sh) & 255) * w01 + ((c11 >> sh) & 255) * w11;
        out |= ((v4 + 32768) >> 16 > 255 ? 255u : (v4 + 32768) >> 16) << sh;
    }
    return out;
}

#ifdef __cplusplus
}
#endif
