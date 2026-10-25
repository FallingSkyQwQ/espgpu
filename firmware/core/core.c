/*
 * core.c — 命令分发、渲染目标、分箱内存、PRESENT
 */

#include <math.h>
#include <stdlib.h>

#include "core_internal.h"

/* ── arena ───────────────────────────────────────────────────────── */

void *gp_arena_alloc(gpu_core_t *c, uint32_t bytes, uint32_t *off_out)
{
    bytes = (bytes + 7u) & ~7u;
    if (c->arena_used + bytes > c->arena_cap) {
        return NULL;
    }
    const uint32_t off = c->arena_used;
    c->arena_used += bytes;
    if (off_out) {
        *off_out = off;
    }
    return c->arena + off;
}

static void bins_reset(gpu_core_t *c)
{
    memset(c->bins, 0, sizeof(c->bins));
    c->arena_used = 16; /* 偏移 0 保留给"无" */
    c->rstate_off = 0;
    c->rstate_2d_off = 0;
    c->consts_off = 0;
    c->pending = false;
    c->bin_epoch++;
}

void gp_send_event(gpu_core_t *c, uint8_t kind, uint16_t value, uint32_t arg)
{
    if (c->cb.send_event == NULL) {
        return;
    }
    espgpu_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    ev.value = value;
    ev.arg = arg;
    c->cb.send_event(c->cb.user, &ev);
}

/* ── 渲染目标 ─────────────────────────────────────────────────────── */

static void target_free(gp_target_t *t)
{
    if (t->color) {
        gp_free(t->color);
    }
    if (t->depth) {
        gp_free(t->depth);
    }
    memset(t, 0, sizeof(*t));
}

static bool target_alloc(gp_target_t *t, uint16_t w, uint16_t h, bool want_fast)
{
    memset(t, 0, sizeof(*t));
    const size_t bytes = (size_t)w * h * 2;
    t->color = want_fast ? (uint16_t *)gp_alloc_fast(bytes) : NULL;
    if (t->color == NULL) {
        t->color = (uint16_t *)gp_alloc_big(bytes);
    }
    if (t->color == NULL) {
        return false;
    }
    memset(t->color, 0, bytes);
    t->color_fast = (uint8_t)gp_is_fast(t->color);
    t->w = w;
    t->h = h;
    return true;
}

bool gp_target_need_depth(gpu_core_t *c, gp_target_t *t)
{
    (void)c;
    if (t->depth) {
        return true;
    }
    const size_t n = (size_t)t->w * t->h;
    t->depth = (uint16_t *)gp_alloc_big(n * 2);
    if (t->depth == NULL) {
        gp_log(GP_LOG_ERROR, "depth buffer alloc failed (%ux%u)", t->w, t->h);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        t->depth[i] = 0xFFFF;
    }
    return true;
}

/** 按当前目标重算视口 / 裁剪的有效值 */
static void update_rects(gpu_core_t *c)
{
    gp_target_t *t = c->target;
    const int W = t->w;
    const int H = t->h;

    if (c->vp_req_full) {
        c->vp_x = 0;
        c->vp_y = 0;
        c->vp_w = (int16_t)W;
        c->vp_h = (int16_t)H;
    } else {
        int y = (int16_t)c->vp_req.y;
        if (t->bottom_up) {
            y = H - (y + (int16_t)c->vp_req.h);
        }
        c->vp_x = (int16_t)c->vp_req.x;
        c->vp_y = (int16_t)y;
        c->vp_w = (int16_t)c->vp_req.w;
        c->vp_h = (int16_t)c->vp_req.h;
    }

    int x0 = 0, y0 = 0, x1 = W, y1 = H;
    if (!c->sc_req_full) {
        int sx = (int16_t)c->sc_req.x;
        int sy = (int16_t)c->sc_req.y;
        const int sw = (int16_t)c->sc_req.w;
        const int sh = (int16_t)c->sc_req.h;
        if (t->bottom_up) {
            sy = H - (sy + sh);
        }
        x0 = sx;
        y0 = sy;
        x1 = sx + sw;
        y1 = sy + sh;
    }
    x0 = gp_clampi(x0, 0, W);
    y0 = gp_clampi(y0, 0, H);
    x1 = gp_clampi(x1, 0, W);
    y1 = gp_clampi(y1, 0, H);
    c->sc_x = (int16_t)x0;
    c->sc_y = (int16_t)y0;
    c->sc_w = (int16_t)(x1 > x0 ? x1 - x0 : 0);
    c->sc_h = (int16_t)(y1 > y0 ? y1 - y0 : 0);
}

static void set_target(gpu_core_t *c, gp_target_t *t)
{
    c->target = t;
    c->tiles_x = (t->w + GP_TILE - 1) / GP_TILE;
    c->tiles_y = (t->h + GP_TILE - 1) / GP_TILE;
    update_rects(c);
}

/** FBO 目标 → 纹理（行翻转：纹理第 0 行 = 目标最底一行，即 GL 的 t=0） */
static void resolve_fbo(gpu_core_t *c, gp_target_t *t)
{
    gp_tex_t *tex = gp_tex_find(&c->tc, t->tex_id);
    if (tex == NULL || tex->px == NULL || tex->w != t->w || tex->h != t->h) {
        tex = gp_tex_make_rgb565(c, t->tex_id, t->w, t->h);
    }
    if (tex == NULL) {
        return;
    }
    for (int y = 0; y < t->h; y++) {
        memcpy(tex->px + (size_t)y * t->w, t->color + (size_t)(t->h - 1 - y) * t->w,
               (size_t)t->w * 2);
    }
    tex->is_target = 0;
}

static gp_target_t *fbo_slot(gpu_core_t *c, uint32_t tex_id, uint16_t w, uint16_t h)
{
    gp_target_t *free_slot = NULL;
    for (int i = 0; i < GP_MAX_TARGETS; i++) {
        gp_target_t *t = &c->fbo[i];
        if (t->color && t->tex_id == tex_id) {
            if (t->w == w && t->h == h) {
                return t;
            }
            target_free(t);
            free_slot = t;
            break;
        }
        if (t->color == NULL && free_slot == NULL) {
            free_slot = t;
        }
    }
    if (free_slot == NULL) {
        /* 槽位满：回收第一个不是当前目标的 */
        for (int i = 0; i < GP_MAX_TARGETS; i++) {
            if (&c->fbo[i] != c->target) {
                target_free(&c->fbo[i]);
                free_slot = &c->fbo[i];
                break;
            }
        }
    }
    if (free_slot == NULL || !target_alloc(free_slot, w, h, false)) {
        return NULL;
    }
    free_slot->tex_id = tex_id;
    free_slot->bottom_up = 1;
    return free_slot;
}

static void cmd_fbo(gpu_core_t *c, const espgpu_fbo_t *f)
{
    switch (f->action) {
    case ESPGPU_FBO_BIND_TEXTURE: {
        if (f->texture_id == 0) {
            break;
        }
        gp_flush(c);
        if (c->target != &c->screen) {
            resolve_fbo(c, c->target);
        }
        uint16_t w = f->width, h = f->height;
        gp_tex_t *tex = gp_tex_find(&c->tc, f->texture_id);
        if ((w == 0 || h == 0) && tex) {
            w = tex->w;
            h = tex->h;
        }
        if (w == 0 || h == 0 || w > GP_MAX_W || h > GP_MAX_H) {
            gp_log(GP_LOG_WARN, "fbo: bad size %ux%u for tex %u", w, h, (unsigned)f->texture_id);
            break;
        }
        gp_target_t *t = fbo_slot(c, f->texture_id, w, h);
        if (t == NULL) {
            gp_log(GP_LOG_ERROR, "fbo: alloc failed %ux%u", w, h);
            break;
        }
        /* 纹理已有内容就带进来（GL 里 FBO 纹理的旧内容是可见的） */
        if (tex && tex->px && tex->w == w && tex->h == h) {
            for (int y = 0; y < h; y++) {
                memcpy(t->color + (size_t)(h - 1 - y) * w, tex->px + (size_t)y * w,
                       (size_t)w * 2);
            }
        }
        if (tex) {
            tex->is_target = 1;
        }
        set_target(c, t);
        break;
    }
    case ESPGPU_FBO_BIND_DEFAULT:
        if (c->target != &c->screen) {
            gp_flush(c);
            resolve_fbo(c, c->target);
            set_target(c, &c->screen);
        }
        break;
    case ESPGPU_FBO_COPY_TO_TEXTURE: {
        gp_flush(c);
        gp_target_t *t = c->target;
        int w = f->width, h = f->height;
        int x = f->x;
        int y = f->y;
        if (t->bottom_up) {
            y = t->h - (y + h);
        }
        /* 尺寸上限：32 位 size_t 上 w*h*2 会回绕成小分配再整块写入 */
        if (w <= 0 || h <= 0 || f->texture_id == 0 || w > c->cfg.max_texture_dim ||
            h > c->cfg.max_texture_dim) {
            break;
        }
        gp_tex_t *tex = gp_tex_make_rgb565(c, f->texture_id, (uint16_t)w, (uint16_t)h);
        if (tex == NULL) {
            break;
        }
        for (int r = 0; r < h; r++) {
            const int sy = y + h - 1 - r;
            for (int col = 0; col < w; col++) {
                const int sx = x + col;
                uint16_t v = 0;
                if (sx >= 0 && sy >= 0 && sx < t->w && sy < t->h) {
                    v = t->color[(size_t)sy * t->w + sx];
                }
                tex->px[(size_t)r * w + col] = v;
            }
        }
        c->rstate_off = 0;
        c->rstate_2d_off = 0;
        break;
    }
    default:
        break;
    }
}

/* ── 分箱 ─────────────────────────────────────────────────────────── */

void gp_bin_tile(gpu_core_t *c, int tile, uint32_t rec_off)
{
    gp_bin_t *b = &c->bins[tile];
    gp_bin_chunk_t *ch = b->tail ? (gp_bin_chunk_t *)gp_arena_ptr(c, b->tail) : NULL;
    if (ch == NULL || ch->count == GP_BIN_CHUNK) {
        uint32_t off;
        gp_bin_chunk_t *n = (gp_bin_chunk_t *)gp_arena_alloc(c, sizeof(gp_bin_chunk_t), &off);
        if (n == NULL) {
            /* 调用者已经按最坏情况预留过空间，这里不应发生 */
            gp_log(GP_LOG_ERROR, "bin: arena exhausted");
            return;
        }
        n->next = 0;
        n->count = 0;
        if (ch) {
            ch->next = off;
        } else {
            b->head = off;
        }
        b->tail = off;
        ch = n;
    }
    ch->rec[ch->count++] = rec_off;
    b->count++;
}

void gp_bin_record(gpu_core_t *c, uint32_t rec_off, int x0, int y0, int x1, int y1)
{
    const int tx0 = x0 >> GP_TILE_SHIFT;
    const int ty0 = y0 >> GP_TILE_SHIFT;
    const int tx1 = x1 >> GP_TILE_SHIFT;
    const int ty1 = y1 >> GP_TILE_SHIFT;

    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            gp_bin_tile(c, ty * c->tiles_x + tx, rec_off);
        }
    }
    c->pending = true;
}

/** 剩余空间是否足够放一个 rec_bytes 的记录并覆盖 n_tiles 个 tile */
static bool arena_fits(gpu_core_t *c, uint32_t rec_bytes, uint32_t n_tiles)
{
    const uint32_t need = ((rec_bytes + 7u) & ~7u) + n_tiles * (uint32_t)sizeof(gp_bin_chunk_t) +
                          (uint32_t)sizeof(gp_rstate_t) + ESPGPU_VM_CONSTS * 16u + 64u;
    return c->arena_used + need <= c->arena_cap;
}

bool gp_reserve(gpu_core_t *c, uint32_t rec_bytes, uint32_t n_tiles)
{
    if (arena_fits(c, rec_bytes, n_tiles)) {
        return true;
    }
    gp_flush(c);
    return arena_fits(c, rec_bytes, n_tiles);
}

void gp_bin_clear(gpu_core_t *c, uint8_t flags, uint16_t color, uint16_t depth)
{
    if (c->sc_w <= 0 || c->sc_h <= 0 || (flags & 3) == 0) {
        return;
    }
    if ((flags & 2) && !gp_target_need_depth(c, c->target)) {
        flags &= (uint8_t)~2;
    }
    const int x0 = c->sc_x, y0 = c->sc_y;
    const int x1 = c->sc_x + c->sc_w - 1, y1 = c->sc_y + c->sc_h - 1;
    const uint32_t n_tiles =
        (uint32_t)(((x1 >> GP_TILE_SHIFT) - (x0 >> GP_TILE_SHIFT) + 1) *
                   ((y1 >> GP_TILE_SHIFT) - (y0 >> GP_TILE_SHIFT) + 1));

    if (!gp_reserve(c, sizeof(gp_clear_t), n_tiles)) {
        return;
    }
    uint32_t off;
    gp_clear_t *r = (gp_clear_t *)gp_arena_alloc(c, sizeof(gp_clear_t), &off);
    r->type = GP_REC_CLEAR;
    r->flags = flags;
    r->color = color;
    r->depth = depth;
    r->x0 = (int16_t)x0;
    r->y0 = (int16_t)y0;
    r->x1 = (int16_t)x1;
    r->y1 = (int16_t)y1;
    gp_bin_record(c, off, x0, y0, x1, y1);
}

/* ── 状态快照 ─────────────────────────────────────────────────────── */

static void sampler_from_binding(gpu_core_t *c, int unit, gp_sampler_t *s)
{
    memset(s, 0, sizeof(*s));
    const uint32_t id = c->bind_id[unit];
    if (id == 0) {
        return;
    }
    gp_tex_t *t = gp_tex_find(&c->tc, id);
    if (t == NULL || (t->px == NULL && t->idx == NULL)) {
        c->tc.misses++;
        return;
    }
    t->last_used = c->frame;
    t->bin_epoch = c->bin_epoch;
    s->tex = t;
    s->filter = c->bind_filter[unit];
    s->wrap_u = c->bind_wrap_u[unit];
    s->wrap_v = c->bind_wrap_v[unit];
}

static void blend_factors_2d(uint8_t mode, gp_rstate_t *s)
{
    switch (mode) {
    case ESPGPU_BLEND_ALPHA:
        s->blend = 1;
        s->bsrc = ESPGPU_BLEND_SRC_ALPHA;
        s->bdst = ESPGPU_BLEND_ONE_MINUS_SRC_ALPHA;
        break;
    case ESPGPU_BLEND_ADD:
        s->blend = 1;
        s->bsrc = ESPGPU_BLEND_SRC_ALPHA;
        s->bdst = ESPGPU_BLEND_ONE;
        break;
    case ESPGPU_BLEND_MUL:
        s->blend = 1;
        s->bsrc = ESPGPU_BLEND_DST_COLOR;
        s->bdst = ESPGPU_BLEND_ZERO;
        break;
    default:
        s->blend = 0;
        break;
    }
    s->bop = ESPGPU_BLEND_OP_ADD;
}

uint32_t gp_snapshot_state(gpu_core_t *c, bool for_2d)
{
    uint32_t *cache = for_2d ? &c->rstate_2d_off : &c->rstate_off;
    if (*cache) {
        return *cache;
    }

    uint32_t off;
    gp_rstate_t *s = (gp_rstate_t *)gp_arena_alloc(c, sizeof(gp_rstate_t), &off);
    if (s == NULL) {
        return 0;
    }
    memset(s, 0, sizeof(*s));

    if (for_2d) {
        const espgpu_pipeline2d_t *p = &c->p2d;
        sampler_from_binding(c, 0, &s->smp[0]);
        if (s->smp[0].tex) {
            /* 2D 路径：管线的 filter / wrap 覆盖绑定时的值（与 M2 行为一致） */
            s->smp[0].filter = p->filter;
            s->smp[0].wrap_u = p->wrap;
            s->smp[0].wrap_v = p->wrap;
        }
        s->shade = ESPGPU_SHADE_TEX_MOD_COLOR; /* 2D 顶点色 0xFFFF = 白，等价于不调制 */
        blend_factors_2d(p->blend, s);
        s->alpha_test = p->alpha_test;
        s->alpha_func = ESPGPU_CMP_GEQUAL;
        s->alpha_ref = p->alpha_ref;
        s->color_write = 1;
    } else {
        const espgpu_pipeline_t *p = &c->ps;
        s->shade = p->shade_mode;
        s->blend = p->blend_enable;
        s->bsrc = p->blend_src;
        s->bdst = p->blend_dst;
        s->bop = p->blend_op;
        s->depth_test = p->depth_test;
        s->depth_write = p->depth_write;
        s->depth_func = p->depth_func;
        s->alpha_test = p->alpha_test;
        s->alpha_func = p->alpha_func;
        s->alpha_ref = p->alpha_ref;
        s->fog = p->fog_enable;
        const uint32_t fc = gp_expand565(p->fog_color);
        s->fog_r = (uint8_t)(fc >> 16);
        s->fog_g = (uint8_t)(fc >> 8);
        s->fog_b = (uint8_t)fc;
        s->color_write = c->ext.color_write;
        for (int u = 0; u < (int)ESPGPU_MAX_TEXTURE_UNITS; u++) {
            sampler_from_binding(c, u, &s->smp[u]);
        }
        if (s->shade == ESPGPU_SHADE_PROGRAM) {
            const gp_prog_t *pr = c->ext.program_id < ESPGPU_VM_MAX_PROGRAMS
                                      ? c->progs[c->ext.program_id]
                                      : NULL;
            if (pr && pr->valid) {
                s->vm = 1;
                s->prog = pr;
                if (c->consts_off == 0) {
                    float *k = (float *)gp_arena_alloc(c, sizeof(c->consts), &c->consts_off);
                    if (k) {
                        memcpy(k, c->consts, sizeof(c->consts));
                    }
                }
                s->consts = c->consts_off ? (const float *)gp_arena_ptr(c, c->consts_off) : NULL;
            } else {
                s->shade = ESPGPU_SHADE_FLAT; /* 程序无效：退化成顶点色 */
            }
        }
        if (s->depth_test || s->depth_write) {
            if (!gp_target_need_depth(c, c->target)) {
                s->depth_test = 0;
                s->depth_write = 0;
            }
        }
        if (!s->depth_test) {
            /* GL：关闭深度测试时也不写深度 */
            s->depth_write = 0;
        }
    }

    *cache = off;
    return off;
}

/* ── 刷新与呈现 ───────────────────────────────────────────────────── */

void gp_flush(gpu_core_t *c)
{
    if (!c->pending) {
        return;
    }
    gp_raster_frame(c);
    c->st_flushes++;
    if (c->arena_used > c->st_bin_peak) {
        c->st_bin_peak = c->arena_used;
    }
    bins_reset(c);
}

static void do_present(gpu_core_t *c)
{
    if (c->target != &c->screen) {
        /* 主机忘了解绑 FBO：先把 FBO 收尾，再回屏幕 */
        gp_flush(c);
        resolve_fbo(c, c->target);
        set_target(c, &c->screen);
    }
    gp_flush(c);

    const uint16_t *px = c->screen.color;
    if (c->debug_view != ESPGPU_DEBUG_VIEW_NONE) {
        gp_compose_debug(c);
        if (c->dbg_img) {
            px = c->dbg_img;
        }
    }
    if (c->cb.present && px) {
        c->cb.present(c->cb.user, px, c->screen.w, c->screen.h);
    }

    {
        /* 分段计时：周期 → µs（每帧一次除法） */
        const uint64_t r = gp_cycles_per_us();
        c->last_vtx_us = (uint32_t)(c->cy_vtx / r);
        c->last_clip_us = (uint32_t)(c->cy_clip / r);
        c->last_setup_us = (uint32_t)(c->cy_setup / r);
        c->last_bin_us = (uint32_t)(c->cy_bin / r);
        c->last_tile_us = (uint32_t)(c->cy_raster / r);
        c->last_io_us = (uint32_t)(c->cy_io / r);
        c->last_px_seg_us = (uint32_t)(c->cy_px_seg / r);
        c->last_px_pix_us = (uint32_t)(c->cy_px_pix / r);
        c->last_px_rows = c->st_px_rows;
        c->last_px_segs = c->st_px_segs;
        c->last_px_tested = c->st_px_tested;
        c->cy_px_seg = c->cy_px_pix = 0;
        c->st_px_rows = c->st_px_segs = c->st_px_tested = 0;
        c->cy_vtx = c->cy_clip = c->cy_setup = c->cy_bin = c->cy_raster = c->cy_io = 0;
        memcpy(c->last_path, c->st_path, sizeof(c->last_path));
        memset(c->st_path, 0, sizeof(c->st_path));
        c->last_flushes = c->st_flushes;
        c->st_flushes = 0;
    }
    c->last_tris = c->st_tris;
    c->last_culled = c->st_culled;
    c->last_bin = c->st_bin_peak;
    c->last_tnl_us = c->st_tnl_us;
    c->last_raster_us = c->st_raster_us;
    c->st_raster_us = 0;
    c->st_tris = 0;
    c->st_culled = 0;
    c->st_bin_peak = 0;
    c->st_tnl_us = 0;
    if (c->dbg) {
        memset(c->dbg, 0, (size_t)c->screen.w * c->screen.h);
    }
    memset(c->dbg_tile_tris, 0, sizeof(c->dbg_tile_tris));
    c->frame++;
    c->last_fill_frame = c->last_fill;
    c->last_fill = 0;
}

/* ── 生命周期 ─────────────────────────────────────────────────────── */

static void reset_state(gpu_core_t *c)
{
    memset(&c->ps, 0, sizeof(c->ps));
    c->ps.shade_mode = ESPGPU_SHADE_FLAT;
    c->ps.depth_func = ESPGPU_CMP_LESS;
    c->ps.depth_write = 1;
    c->ps.alpha_func = ESPGPU_CMP_ALWAYS;
    c->ps.blend_src = ESPGPU_BLEND_ONE;
    c->ps.blend_dst = ESPGPU_BLEND_ZERO;
    c->ps.cull_mode = ESPGPU_CULL_NONE;
    c->ps.front_face = ESPGPU_FRONT_CCW;
    memset(&c->ext, 0, sizeof(c->ext));
    c->ext.color_write = 1;
    c->ext.shade_model = ESPGPU_SHADE_MODEL_SMOOTH;
    c->fog.fog_start = 0.0f;
    c->fog.fog_end = 1.0f;
    c->fog.fog_density = 1.0f;
    for (int i = 0; i < ESPGPU_MATRIX_COUNT; i++) {
        memset(c->mat[i], 0, sizeof(c->mat[i]));
        c->mat[i][0] = c->mat[i][5] = c->mat[i][10] = c->mat[i][15] = 1.0f;
        c->mat_set[i] = false;
    }
    c->const_color[0] = c->const_color[1] = c->const_color[2] = c->const_color[3] = 255;
    c->depth_n = 0.0f;
    c->depth_f = 1.0f;
    c->vp_req_full = true;
    c->sc_req_full = true;
    memset(&c->p2d, 0, sizeof(c->p2d));
    c->p2d.wrap = ESPGPU_WRAP_CLAMP;
    for (int u = 0; u < (int)ESPGPU_MAX_TEXTURE_UNITS; u++) {
        c->bind_id[u] = 0;
        c->bind_filter[u] = ESPGPU_FILTER_NEAREST;
        c->bind_wrap_u[u] = ESPGPU_WRAP_REPEAT;
        c->bind_wrap_v[u] = ESPGPU_WRAP_REPEAT;
    }
    memset(&c->light_hdr, 0, sizeof(c->light_hdr));
    c->layout_valid = false;
}

gpu_core_t *gpu_core_create(const gpu_core_config_t *cfg, const gpu_core_cb_t *cb)
{
    /* 约 7.5 KB（含每个 worker 1.3 KB 的雾表），每个热路径都在读（状态、分箱、worker、矩阵）：
     * 放内部 SRAM，失败退回 PSRAM */
    gpu_core_t *c = (gpu_core_t *)gp_alloc_fast(sizeof(gpu_core_t));
    if (c == NULL) {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    if (cb) {
        c->cb = *cb;
    }
    if (c->cfg.max_texture_dim == 0) {
        c->cfg.max_texture_dim = 512;
    }

    c->arena_cap = cfg->arena_bytes ? cfg->arena_bytes : (1u << 20);
    c->arena = (uint8_t *)gp_alloc_big(c->arena_cap);
    c->vcache = gp_alloc_fast(sizeof(*c->vcache) * GP_VCACHE);
    c->buffers = (gp_buffer_t *)gp_alloc_big(sizeof(gp_buffer_t) * ESPGPU_MAX_BUFFERS);
    if (c->arena == NULL || c->vcache == NULL || c->buffers == NULL ||
        !gp_tc_init(&c->tc, cfg->texture_budget ? cfg->texture_budget : (4u << 20))) {
        gpu_core_destroy(c);
        return NULL;
    }
    memset(c->buffers, 0, sizeof(gp_buffer_t) * ESPGPU_MAX_BUFFERS);

    c->nworkers = gp_worker_count();
    if (c->nworkers < 1) {
        c->nworkers = 1;
    }
    if (c->nworkers > GP_MAX_WORKERS) {
        c->nworkers = GP_MAX_WORKERS;
    }
    for (int i = 0; i < c->nworkers; i++) {
        c->workers[i].color = (uint16_t *)gp_alloc_fast(GP_TILE * GP_TILE * 2);
        c->workers[i].depth = (uint16_t *)gp_alloc_fast(GP_TILE * GP_TILE * 2);
        if (c->workers[i].color == NULL || c->workers[i].depth == NULL) {
            gpu_core_destroy(c);
            return NULL;
        }
    }

    reset_state(c);
    bins_reset(c);
    c->target = &c->screen;
    if (!gpu_core_set_size(c, 320, 200)) {
        gpu_core_destroy(c);
        return NULL;
    }
    return c;
}

void gpu_core_destroy(gpu_core_t *c)
{
    if (c == NULL) {
        return;
    }
    target_free(&c->screen);
    for (int i = 0; i < GP_MAX_TARGETS; i++) {
        target_free(&c->fbo[i]);
    }
    for (int i = 0; i < GP_MAX_WORKERS; i++) {
        if (c->workers[i].color) gp_free(c->workers[i].color);
        if (c->workers[i].depth) gp_free(c->workers[i].depth);
    }
    for (int i = 0; i < (int)ESPGPU_VM_MAX_PROGRAMS; i++) {
        if (c->progs[i]) {
            gp_vm_forget(c->progs[i]->code); /* vm.c 按 code 地址缓存解码结果 */
            gp_free(c->progs[i]);
        }
    }
    if (c->buffers) {
        for (uint32_t i = 0; i < ESPGPU_MAX_BUFFERS; i++) {
            if (c->buffers[i].data) gp_free(c->buffers[i].data);
        }
        gp_free(c->buffers);
    }
    gp_tc_destroy(&c->tc);
    if (c->arena) gp_free(c->arena);
    if (c->vcache) gp_free(c->vcache);
    if (c->dbg) gp_free(c->dbg);
    if (c->dbg_img) gp_free(c->dbg_img);
    gp_free(c);
}

static void dbg_ensure(gpu_core_t *c);

bool gpu_core_set_size(gpu_core_t *c, uint16_t w, uint16_t h)
{
    if (w == 0 || h == 0 || w > GP_MAX_W || h > GP_MAX_H) {
        return false;
    }
    bins_reset(c);
    if (c->target != &c->screen) {
        set_target(c, &c->screen);
    }
    if (c->screen.w == w && c->screen.h == h && c->screen.color) {
        return true;
    }
    target_free(&c->screen);
    if (c->dbg) {
        gp_free(c->dbg);
        c->dbg = NULL;
    }
    if (c->dbg_img) {
        gp_free(c->dbg_img);
        c->dbg_img = NULL;
    }
    if (!target_alloc(&c->screen, w, h, true)) {
        gp_log(GP_LOG_ERROR, "screen target alloc failed %ux%u", w, h);
        return false;
    }
    gp_log(GP_LOG_INFO, "screen %ux%u (%s)", w, h,
           c->screen.color_fast ? "fast memory, direct raster" : "slow memory, tiled copy");
    set_target(c, &c->screen);
    dbg_ensure(c); /* 调试视图开着时按新尺寸重建，否则改分辨率后视图静默失效 */
    return true;
}

/** 调试视图需要的屏幕大小缓冲（改分辨率后也要重建） */
static void dbg_ensure(gpu_core_t *c)
{
    if (c->debug_view == ESPGPU_DEBUG_VIEW_NONE || c->screen.color == NULL) {
        return;
    }
    const size_t n = (size_t)c->screen.w * c->screen.h;
    if (c->dbg == NULL) {
        c->dbg = (uint8_t *)gp_alloc_big(n);
        if (c->dbg) {
            memset(c->dbg, 0, n);
        }
    }
    if (c->dbg_img == NULL) {
        c->dbg_img = (uint16_t *)gp_alloc_big(n * 2);
    }
}

void gpu_core_set_debug_view(gpu_core_t *c, uint8_t view)
{
    c->debug_view = view < ESPGPU_DEBUG_VIEW_COUNT ? view : 0;
    dbg_ensure(c);
}

uint32_t gpu_core_frame_counter(gpu_core_t *c)
{
    return c->frame;
}

void gpu_core_fill_stats(gpu_core_t *c, espgpu_stats_t *s)
{
    s->draw_calls = c->st_draws;
    s->primitives = c->st_prims;
    s->vertices = c->st_verts;
    s->fill_pixels = c->last_fill_frame;
    s->t_raster_us = c->last_raster_us;
    s->tex_resident = c->tc.count;
    s->tex_bytes = c->tc.bytes;
    s->tex_evictions = c->tc.evictions;
    s->tex_misses = c->tc.misses;
    s->triangles = c->last_tris;
    s->culled = c->last_culled;
    s->bin_bytes = c->last_bin;
    s->t_tnl_us = c->last_tnl_us;
    s->debug_view = c->debug_view;
    s->buffers_bytes = c->buffer_bytes;
    s->t_vtx_us = c->last_vtx_us;
    s->t_clip_us = c->last_clip_us;
    s->t_setup_us = c->last_setup_us;
    s->t_bin_us = c->last_bin_us;
    s->t_tile_us = c->last_tile_us;
    s->t_tile_io_us = c->last_io_us;
    s->tris_fill = c->last_path[GP_PATH_FILL];
    s->tris_fast = c->last_path[GP_PATH_FAST];
    s->tris_px = c->last_path[GP_PATH_PX];
    s->tris_generic = c->last_path[GP_PATH_GENERIC];
    s->t_px_seg_us = c->last_px_seg_us;
    s->t_px_pix_us = c->last_px_pix_us;
    s->px_rows = c->last_px_rows;
    s->px_segs = c->last_px_segs;
    s->px_tested = c->last_px_tested;
    s->raster_passes = c->last_flushes;
}

/* ── DATA chunk：设备端缓冲 ───────────────────────────────────────── */

static gp_buffer_t *buffer_find(gpu_core_t *c, uint32_t id, bool create)
{
    gp_buffer_t *free_slot = NULL;
    for (uint32_t i = 0; i < ESPGPU_MAX_BUFFERS; i++) {
        if (c->buffers[i].id == id && id != 0) {
            return &c->buffers[i];
        }
        if (c->buffers[i].id == 0 && free_slot == NULL) {
            free_slot = &c->buffers[i];
        }
    }
    if (create && free_slot) {
        free_slot->id = id;
        free_slot->len = 0;
        free_slot->data = NULL;
        return free_slot;
    }
    return NULL;
}

static void buffer_release(gpu_core_t *c, gp_buffer_t *b)
{
    if (b->data) {
        gp_free(b->data);
        c->buffer_bytes -= b->len;
    }
    memset(b, 0, sizeof(*b));
}

void gpu_core_data(gpu_core_t *c, const uint8_t *payload, uint32_t len)
{
    if (len < sizeof(espgpu_data_hdr_t)) {
        return;
    }
    espgpu_data_hdr_t h;
    memcpy(&h, payload, sizeof(h));
    const uint8_t *data = payload + sizeof(h);
    const uint32_t n = len - (uint32_t)sizeof(h);

    if (h.kind != ESPGPU_DATA_BUFFER || h.buffer_id == 0) {
        return;
    }

    if (h.flags & ESPGPU_DATAF_DELETE) {
        gp_buffer_t *b = buffer_find(c, h.buffer_id, false);
        if (b) {
            buffer_release(c, b);
        }
        return;
    }

    gp_buffer_t *b = buffer_find(c, h.buffer_id, h.offset == 0);
    if (b == NULL) {
        gp_send_event(c, ESPGPU_EVT_BUFFER_EVICTED, 0, h.buffer_id);
        return;
    }
    if (h.offset == 0) {
        if (b->data && b->len != h.total_len) {
            gp_free(b->data);
            c->buffer_bytes -= b->len;
            b->data = NULL;
            b->len = 0;
        }
        if (b->data == NULL) {
            if (c->buffer_bytes + h.total_len > c->cfg.buffer_budget ||
                (b->data = (uint8_t *)gp_alloc_big(h.total_len ? h.total_len : 1)) == NULL) {
                gp_log(GP_LOG_WARN, "buffer %u: no memory for %u bytes", (unsigned)h.buffer_id,
                       (unsigned)h.total_len);
                memset(b, 0, sizeof(*b));
                gp_send_event(c, ESPGPU_EVT_BUFFER_EVICTED, 0, h.buffer_id);
                return;
            }
            b->len = h.total_len;
            c->buffer_bytes += b->len;
        }
    }
    if (b->data == NULL || h.offset > b->len || n > b->len - h.offset) {
        return;
    }
    memcpy(b->data + h.offset, data, n);
}

const uint8_t *gp_buffer_data(gpu_core_t *c, uint32_t id, uint32_t *len)
{
    gp_buffer_t *b = buffer_find(c, id, false);
    if (b == NULL || b->data == NULL) {
        return NULL;
    }
    *len = b->len;
    return b->data;
}

/* ── 命令执行 ─────────────────────────────────────────────────────── */

static void mat_load(gpu_core_t *c, const espgpu_set_matrix_t *m, const uint8_t *data,
                     uint32_t len)
{
    if (m->slot >= ESPGPU_MATRIX_COUNT) {
        return;
    }
    float *dst = c->mat[m->slot];
    if (m->flags & ESPGPU_MATF_IDENTITY) {
        memset(dst, 0, 16 * sizeof(float));
        dst[0] = dst[5] = dst[10] = dst[15] = 1.0f;
    } else {
        if (len < 64) {
            return;
        }
        float tmp[16];
        for (int i = 0; i < 16; i++) {
            if (m->flags & ESPGPU_MATF_FLOAT) {
                memcpy(&tmp[i], data + i * 4, 4);
            } else {
                int32_t v;
                memcpy(&v, data + i * 4, 4);
                tmp[i] = (float)v / 65536.0f;
            }
        }
        if (m->flags & ESPGPU_MATF_TRANSPOSE) {
            for (int r = 0; r < 4; r++) {
                for (int col = 0; col < 4; col++) {
                    dst[r * 4 + col] = tmp[col * 4 + r];
                }
            }
        } else {
            memcpy(dst, tmp, sizeof(tmp));
        }
    }
    c->mat_set[m->slot] = true;
}

static void apply_pipeline(gpu_core_t *c, const uint8_t *body, uint32_t size)
{
    espgpu_pipeline_t p;
    memcpy(&p, body, sizeof(p));
    espgpu_pipeline_t *s = &c->ps;
    const uint16_t m = p.mask;

    if (m & ESPGPU_PS_BLEND) {
        s->blend_enable = p.blend_enable;
        s->blend_src = p.blend_src;
        s->blend_dst = p.blend_dst;
        s->blend_op = p.blend_op;
    }
    if (m & ESPGPU_PS_DEPTH) {
        s->depth_test = p.depth_test;
        s->depth_write = p.depth_write;
        s->depth_func = p.depth_func;
    }
    if (m & ESPGPU_PS_ALPHA_TEST) {
        s->alpha_test = p.alpha_test;
        s->alpha_func = p.alpha_func;
        s->alpha_ref = p.alpha_ref;
    }
    if (m & ESPGPU_PS_CULL) {
        s->cull_mode = p.cull_mode;
        s->front_face = p.front_face;
    }
    if (m & ESPGPU_PS_SHADE) {
        s->shade_mode = p.shade_mode;
        s->blend_shade = p.blend_shade;
    }
    if (m & ESPGPU_PS_FOG) {
        s->fog_enable = p.fog_enable;
        s->fog_mode = p.fog_mode;
        s->fog_color = p.fog_color;
    }
    if (m & ESPGPU_PS_TEXTURE) {
        const int n = p.tex_count < ESPGPU_MAX_TEXTURE_UNITS ? p.tex_count
                                                              : ESPGPU_MAX_TEXTURE_UNITS;
        for (int i = 0; i < n; i++) {
            const uint8_t u = p.tex_unit[i];
            if (u >= ESPGPU_MAX_TEXTURE_UNITS) {
                continue;
            }
            c->bind_id[u] = p.tex_id[i];
            c->bind_filter[u] = p.tex_filter[i];
            c->bind_wrap_u[u] = p.tex_wrap_u[i];
            c->bind_wrap_v[u] = p.tex_wrap_v[i];
        }
    }

    uint32_t off = sizeof(p);
    if (size >= off + sizeof(espgpu_pipeline_fog_t)) {
        if (m & ESPGPU_PS_FOG) {
            memcpy(&c->fog, body + off, sizeof(c->fog));
        }
        off += sizeof(espgpu_pipeline_fog_t);
        if (size >= off + sizeof(espgpu_pipeline_ext_t)) {
            memcpy(&c->ext, body + off, sizeof(c->ext));
        }
    }
}

static void exec_one(gpu_core_t *c, const espgpu_cmd_t *cmd, const uint8_t *body)
{
    const uint32_t size = cmd->size;

    switch (cmd->op) {
    case ESPGPU_OP_NOP:
    case ESPGPU_OP_QUERY:
    case ESPGPU_OP_SET_CLUT:
        break;

    case ESPGPU_OP_PIPELINE:
        if (size >= sizeof(espgpu_pipeline_t)) {
            apply_pipeline(c, body, size);
            c->rstate_off = 0;
            c->rstate_2d_off = 0; /* PS_TEXTURE 改的绑定 2D 路径也在用 */
        }
        break;

    case ESPGPU_OP_SET_MATRIX:
        if (size >= sizeof(espgpu_set_matrix_t)) {
            espgpu_set_matrix_t m;
            memcpy(&m, body, sizeof(m));
            mat_load(c, &m, body + sizeof(m), size - (uint32_t)sizeof(m));
        }
        break;

    case ESPGPU_OP_VERTEX_LAYOUT:
        if (size >= 4) {
            memset(&c->layout, 0, sizeof(c->layout));
            memcpy(&c->layout, body, size < sizeof(c->layout) ? size : sizeof(c->layout));
            if (c->layout.attrib_count > ESPGPU_MAX_VERTEX_ATTRIBS) {
                c->layout.attrib_count = ESPGPU_MAX_VERTEX_ATTRIBS;
            }
            c->layout_valid = c->layout.attrib_count > 0 && c->layout.stride > 0;
        }
        break;

    case ESPGPU_OP_BIND_TEXTURE:
        if (size >= sizeof(espgpu_bind_texture_t)) {
            espgpu_bind_texture_t b;
            memcpy(&b, body, sizeof(b));
            if (b.unit < ESPGPU_MAX_TEXTURE_UNITS) {
                c->bind_id[b.unit] = b.texture;
                c->bind_filter[b.unit] = b.filter;
                c->bind_wrap_u[b.unit] = b.wrap;
                c->bind_wrap_v[b.unit] =
                    (cmd->flags & ESPGPU_BINDF_WRAP_V) ? (uint8_t)(b.pad & 0xFF) : b.wrap;
                c->rstate_off = 0;
                c->rstate_2d_off = 0;
            }
        }
        break;

    case ESPGPU_OP_UPLOAD_TEXTURE:
        if (size >= sizeof(espgpu_upload_texture_t)) {
            espgpu_upload_texture_t u;
            memcpy(&u, body, sizeof(u));
            gp_tex_upload(c, &u, cmd->flags, body + sizeof(u), size - (uint32_t)sizeof(u));
            c->rstate_off = 0;
            c->rstate_2d_off = 0;
        }
        break;

    case ESPGPU_OP_TEX_PALETTE:
        if (size >= sizeof(espgpu_tex_palette_t)) {
            espgpu_tex_palette_t p;
            memcpy(&p, body, sizeof(p));
            gp_tex_palette(c, &p, body + sizeof(p), size - (uint32_t)sizeof(p));
            c->rstate_off = 0;
            c->rstate_2d_off = 0;
        }
        break;

    case ESPGPU_OP_DELETE_TEXTURE:
        if (size >= sizeof(espgpu_delete_texture_t)) {
            espgpu_delete_texture_t d;
            memcpy(&d, body, sizeof(d));
            if (d.id == ESPGPU_TEXTURE_ALL) {
                gp_tex_delete_all(c);
            } else {
                gp_tex_delete(c, d.id);
            }
            c->rstate_off = 0;
            c->rstate_2d_off = 0;
        }
        break;

    case ESPGPU_OP_DRAW:
        if (size >= sizeof(espgpu_draw_t)) {
            espgpu_draw_t d;
            memcpy(&d, body, sizeof(d));
            const uint64_t t0 = gp_time_us();
            const uint32_t r0 = c->st_raster_us;
            if (cmd->flags & ESPGPU_DRAWF_3D) {
                gp_draw3d(c, &d, body + sizeof(d), size - (uint32_t)sizeof(d));
            } else {
                gp_draw2d(c, &d, body + sizeof(d), size - (uint32_t)sizeof(d));
            }
            /* arena 满时绘制中途会刷新光栅：那部分已记进 t_raster，别再算进 T&L */
            const uint32_t dt = (uint32_t)(gp_time_us() - t0);
            const uint32_t dr = c->st_raster_us - r0;
            c->st_tnl_us += dt > dr ? dt - dr : 0;
            c->st_draws++;
        }
        break;

    case ESPGPU_OP_CLEAR:
        if (size >= 4) {
            espgpu_clear_t cl;
            memset(&cl, 0, sizeof(cl));
            memcpy(&cl, body, size < sizeof(cl) ? size : sizeof(cl));
            if (size < sizeof(cl)) {
                cl.depth = 0xFFFF;
            }
            gp_bin_clear(c, cl.flags, cl.color, (uint16_t)(cl.depth > 0xFFFF ? 0xFFFF : cl.depth));
        }
        break;

    case ESPGPU_OP_VIEWPORT:
        if (size >= sizeof(espgpu_rect_t)) {
            memcpy(&c->vp_req, body, sizeof(espgpu_rect_t));
            c->vp_req_full = false;
            update_rects(c);
        }
        break;

    case ESPGPU_OP_SCISSOR:
        if (size >= sizeof(espgpu_rect_t)) {
            memcpy(&c->sc_req, body, sizeof(espgpu_rect_t));
            c->sc_req_full = false;
            update_rects(c);
        }
        break;

    case ESPGPU_OP_DEPTH_RANGE:
        if (size >= sizeof(espgpu_depth_range_t)) {
            espgpu_depth_range_t r;
            memcpy(&r, body, sizeof(r));
            c->depth_n = gp_clampf(r.znear, 0.0f, 1.0f);
            c->depth_f = gp_clampf(r.zfar, 0.0f, 1.0f);
        }
        break;

    case ESPGPU_OP_FBO:
        if (size >= sizeof(espgpu_fbo_t)) {
            espgpu_fbo_t f;
            memcpy(&f, body, sizeof(f));
            cmd_fbo(c, &f);
        }
        break;

    case ESPGPU_OP_PRESENT:
        do_present(c);
        break;

    case ESPGPU_OP_FENCE:
        if (size >= sizeof(espgpu_fence_t)) {
            espgpu_fence_t f;
            memcpy(&f, body, sizeof(f));
            gp_send_event(c, ESPGPU_EVT_FENCE, 0, f.value);
        }
        break;

    case ESPGPU_OP_PIPELINE2D:
        if (size >= sizeof(espgpu_pipeline2d_t)) {
            memcpy(&c->p2d, body, sizeof(c->p2d));
            c->rstate_2d_off = 0;
        }
        break;

    case ESPGPU_OP_COLOR:
        if (size >= sizeof(espgpu_color_t)) {
            memcpy(c->const_color, body, 4);
        }
        break;

    case ESPGPU_OP_LIGHTING:
        if (size >= sizeof(espgpu_lighting_t)) {
            memcpy(&c->light_hdr, body, sizeof(c->light_hdr));
            uint32_t n = c->light_hdr.count;
            if (n > ESPGPU_MAX_LIGHTS) {
                n = ESPGPU_MAX_LIGHTS;
            }
            const uint32_t avail = (size - (uint32_t)sizeof(espgpu_lighting_t)) /
                                   (uint32_t)sizeof(espgpu_light_t);
            if (n > avail) {
                n = avail;
            }
            memcpy(c->lights, body + sizeof(espgpu_lighting_t), n * sizeof(espgpu_light_t));
            c->light_hdr.count = (uint8_t)n;
        }
        break;

    case ESPGPU_OP_LOAD_PROGRAM:
        if (size >= sizeof(espgpu_load_program_t)) {
            espgpu_load_program_t h;
            memcpy(&h, body, sizeof(h));
            const uint32_t n = h.insn_count;
            if (h.program_id >= ESPGPU_VM_MAX_PROGRAMS || n == 0 || n > ESPGPU_VM_MAX_INSNS ||
                size < sizeof(h) + n * sizeof(espgpu_vm_insn_t)) {
                break;
            }
            const espgpu_vm_insn_t *code = (const espgpu_vm_insn_t *)(body + sizeof(h));
            /* 程序可能正被未刷新的分箱引用 */
            gp_flush(c);
            gp_prog_t *p = c->progs[h.program_id];
            if (p == NULL) {
                p = (gp_prog_t *)gp_alloc_big(sizeof(gp_prog_t));
                c->progs[h.program_id] = p;
            }
            if (p == NULL) {
                break;
            }
            memcpy(p->code, code, n * sizeof(espgpu_vm_insn_t));
            p->count = (uint16_t)n;
            p->varyings = h.varying_count;
            p->valid = gp_vm_validate(p->code, n) ? 1 : 0;
            p->ntemps = p->valid ? gp_vm_temps_used(p->code, n) : 0;
            if (!p->valid) {
                gp_log(GP_LOG_WARN, "program %u rejected by validator", h.program_id);
            }
            c->rstate_off = 0;
        }
        break;

    case ESPGPU_OP_SET_UNIFORMS:
        if (size >= sizeof(espgpu_set_uniforms_t)) {
            espgpu_set_uniforms_t h;
            memcpy(&h, body, sizeof(h));
            uint32_t n = h.count;
            if (h.first >= ESPGPU_VM_CONSTS) {
                break;
            }
            if (h.first + n > ESPGPU_VM_CONSTS) {
                n = ESPGPU_VM_CONSTS - h.first;
            }
            if (size < sizeof(h) + n * 16) {
                n = (size - (uint32_t)sizeof(h)) / 16;
            }
            memcpy(&c->consts[h.first * 4], body + sizeof(h), n * 16);
            c->consts_off = 0;
            c->rstate_off = 0;
        }
        break;

    default:
        break;
    }
}

void gpu_core_exec(gpu_core_t *c, const uint8_t *data, uint32_t len)
{
    uint32_t off = 0;
    while (off + sizeof(espgpu_cmd_t) <= len) {
        espgpu_cmd_t cmd;
        memcpy(&cmd, data + off, sizeof(cmd));
        if (off + sizeof(cmd) + cmd.size > len) {
            gp_log(GP_LOG_WARN, "cmd stream truncated at %u (op 0x%02X size %u)",
                   (unsigned)off, cmd.op, cmd.size);
            return;
        }
        exec_one(c, &cmd, data + off + sizeof(cmd));
        off += (uint32_t)sizeof(cmd) + cmd.size;
    }
}
