/*
 * tex.c — 纹理缓存（PSRAM）+ LRU 换出 + 采样
 *
 * 存储格式：
 *   RGB565    px[]，RGBA4444 上传时展开成 px[] + alpha[]（光栅器只取 16 位 + 8 位）
 *   INDEX8    idx[] + 每纹理调色板 pal[256] / pal_a[256]（GL_EXT_paletted_texture）
 *   R8        idx[]，采样为亮度 (L,L,L,1)
 *
 * LRU：预算超出时换出 last_used 最旧的纹理（PRIORITY 纹理最后考虑），
 * 通过 EVENT 通知主机；正被未刷新分箱引用的纹理先刷新再换出。
 * 主机侧通常维护同样的镜像并提前发 DELETE_TEXTURE，所以这里的换出是兜底。
 */

#include <math.h>

#include "core_internal.h"

#define GP_TEX_TOMB ((gp_tex_t *)(uintptr_t)1)

static inline uint32_t hash_id(uint32_t id)
{
    id ^= id >> 16;
    id *= 0x7feb352du;
    id ^= id >> 15;
    return id;
}

bool gp_tc_init(gp_texcache_t *tc, uint32_t budget)
{
    memset(tc, 0, sizeof(*tc));
    tc->cap = 256;
    tc->slots = (gp_tex_t **)gp_alloc_big(sizeof(gp_tex_t *) * tc->cap);
    if (tc->slots == NULL) {
        return false;
    }
    memset(tc->slots, 0, sizeof(gp_tex_t *) * tc->cap);
    tc->budget = budget;
    return true;
}

static void tex_free_storage(gp_texcache_t *tc, gp_tex_t *t)
{
    if (t->px) gp_free(t->px);
    if (t->idx) gp_free(t->idx);
    if (t->alpha) gp_free(t->alpha);
    if (t->pal) gp_free(t->pal);
    if (t->pal_a) gp_free(t->pal_a);
    t->px = NULL;
    t->idx = NULL;
    t->alpha = NULL;
    t->pal = NULL;
    t->pal_a = NULL;
    tc->bytes -= t->bytes;
    t->bytes = 0;
}

void gp_tc_destroy(gp_texcache_t *tc)
{
    if (tc->slots == NULL) {
        return;
    }
    for (uint32_t i = 0; i < tc->cap; i++) {
        gp_tex_t *t = tc->slots[i];
        if (t && t != GP_TEX_TOMB) {
            tex_free_storage(tc, t);
            gp_free(t);
        }
    }
    gp_free(tc->slots);
    tc->slots = NULL;
}

gp_tex_t *gp_tex_find(gp_texcache_t *tc, uint32_t id)
{
    if (id == 0) {
        return NULL;
    }
    const uint32_t mask = tc->cap - 1;
    uint32_t i = hash_id(id) & mask;
    for (uint32_t n = 0; n < tc->cap; n++, i = (i + 1) & mask) {
        gp_tex_t *t = tc->slots[i];
        if (t == NULL) {
            return NULL;
        }
        if (t != GP_TEX_TOMB && t->id == id) {
            return t;
        }
    }
    return NULL;
}

static bool tc_insert(gp_texcache_t *tc, gp_tex_t *t)
{
    if ((tc->count + tc->tombs + 1) * 10 > tc->cap * 7) {
        /* 重哈希：活跃项多就扩容，否则只是清掉墓碑（否则反复增删后 find 未命中要扫全表） */
        const uint32_t ncap = (tc->count + 1) * 20 > tc->cap * 7 ? tc->cap * 2 : tc->cap;
        gp_tex_t **ns = (gp_tex_t **)gp_alloc_big(sizeof(gp_tex_t *) * ncap);
        if (ns == NULL) {
            return false;
        }
        memset(ns, 0, sizeof(gp_tex_t *) * ncap);
        for (uint32_t i = 0; i < tc->cap; i++) {
            gp_tex_t *o = tc->slots[i];
            if (o && o != GP_TEX_TOMB) {
                uint32_t j = hash_id(o->id) & (ncap - 1);
                while (ns[j]) j = (j + 1) & (ncap - 1);
                ns[j] = o;
            }
        }
        gp_free(tc->slots);
        tc->slots = ns;
        tc->cap = ncap;
        tc->tombs = 0;
    }
    const uint32_t mask = tc->cap - 1;
    uint32_t i = hash_id(t->id) & mask;
    while (tc->slots[i] && tc->slots[i] != GP_TEX_TOMB) {
        i = (i + 1) & mask;
    }
    if (tc->slots[i] == GP_TEX_TOMB) {
        tc->tombs--;
    }
    tc->slots[i] = t;
    tc->count++;
    return true;
}

static void tc_remove(gp_texcache_t *tc, gp_tex_t *t)
{
    const uint32_t mask = tc->cap - 1;
    uint32_t i = hash_id(t->id) & mask;
    for (uint32_t n = 0; n < tc->cap; n++, i = (i + 1) & mask) {
        if (tc->slots[i] == t) {
            tc->slots[i] = GP_TEX_TOMB;
            tc->count--;
            tc->tombs++;
            return;
        }
        if (tc->slots[i] == NULL) {
            return;
        }
    }
}

void gp_tex_before_modify(gpu_core_t *c, gp_tex_t *t)
{
    if (t && c->pending && t->bin_epoch == c->bin_epoch) {
        gp_flush(c);
    }
    c->rstate_off = 0;
    c->rstate_2d_off = 0;
}

static void tex_destroy(gpu_core_t *c, gp_tex_t *t)
{
    gp_tex_before_modify(c, t);
    tc_remove(&c->tc, t);
    tex_free_storage(&c->tc, t);
    gp_free(t);
}

void gp_tex_delete(gpu_core_t *c, uint32_t id)
{
    gp_tex_t *t = gp_tex_find(&c->tc, id);
    if (t) {
        tex_destroy(c, t);
    }
}

void gp_tex_delete_all(gpu_core_t *c)
{
    gp_flush(c);
    gp_texcache_t *tc = &c->tc;
    for (uint32_t i = 0; i < tc->cap; i++) {
        gp_tex_t *t = tc->slots[i];
        if (t && t != GP_TEX_TOMB) {
            tex_free_storage(tc, t);
            gp_free(t);
        }
        tc->slots[i] = NULL;
    }
    tc->count = 0;
    tc->tombs = 0;
    tc->bytes = 0;
    c->rstate_off = 0;
    c->rstate_2d_off = 0;
}

/** 换出最旧的纹理，返回 false = 没得换了 */
static bool evict_one(gpu_core_t *c, const gp_tex_t *keep)
{
    gp_texcache_t *tc = &c->tc;
    gp_tex_t *victim = NULL;
    for (int pass = 0; pass < 2 && victim == NULL; pass++) {
        for (uint32_t i = 0; i < tc->cap; i++) {
            gp_tex_t *t = tc->slots[i];
            if (t == NULL || t == GP_TEX_TOMB || t == keep || t->bytes == 0 || t->is_target) {
                continue;
            }
            if (pass == 0 && t->priority) {
                continue;
            }
            if (victim == NULL || t->last_used < victim->last_used) {
                victim = t;
            }
        }
    }
    if (victim == NULL) {
        return false;
    }
    const uint32_t id = victim->id;
    tex_destroy(c, victim);
    tc->evictions++;
    gp_send_event(c, ESPGPU_EVT_TEXTURE_EVICTED, 0, id);
    return true;
}

static void *tex_alloc(gpu_core_t *c, size_t bytes, const gp_tex_t *keep)
{
    while (c->tc.bytes + bytes > c->tc.budget) {
        if (!evict_one(c, keep)) {
            break;
        }
    }
    void *p = gp_alloc_big(bytes);
    while (p == NULL && evict_one(c, keep)) {
        p = gp_alloc_big(bytes);
    }
    if (p) {
        c->tc.bytes += (uint32_t)bytes;
    }
    return p;
}

gp_tex_t *gp_tex_get(gpu_core_t *c, uint32_t id)
{
    if (id == 0) {
        return NULL; /* 0 = 未绑定；find 永远找不到它，建了就是泄漏 */
    }
    gp_tex_t *t = gp_tex_find(&c->tc, id);
    if (t) {
        return t;
    }
    t = (gp_tex_t *)gp_alloc_big(sizeof(gp_tex_t));
    if (t == NULL) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->id = id;
    t->last_used = c->frame;
    t->bin_epoch = c->bin_epoch - 1;
    if (!tc_insert(&c->tc, t)) {
        gp_free(t);
        return NULL;
    }
    return t;
}

gp_tex_t *gp_tex_make_rgb565(gpu_core_t *c, uint32_t id, uint16_t w, uint16_t h)
{
    gp_tex_t *t = gp_tex_get(c, id);
    if (t == NULL) {
        return NULL;
    }
    gp_tex_before_modify(c, t);
    if (t->px && t->w == w && t->h == h && t->idx == NULL) {
        if (t->alpha) {
            gp_free(t->alpha);
            c->tc.bytes -= (uint32_t)w * h;
            t->bytes -= (uint32_t)w * h;
            t->alpha = NULL;
        }
        t->has_alpha = 0;
        return t;
    }
    tex_free_storage(&c->tc, t);
    const size_t bytes = (size_t)w * h * 2;
    t->px = (uint16_t *)tex_alloc(c, bytes, t);
    if (t->px == NULL) {
        return NULL;
    }
    memset(t->px, 0, bytes);
    t->bytes = (uint32_t)bytes;
    t->w = w;
    t->h = h;
    t->fmt = ESPGPU_FMT_RGB565;
    t->has_alpha = 0;
    t->total_len = (uint32_t)bytes;
    t->received = (uint32_t)bytes;
    return t;
}

static unsigned fmt_bpp(uint8_t fmt)
{
    switch (fmt) {
    case ESPGPU_FMT_RGB565:
    case ESPGPU_FMT_RGBA4444: return 2;
    case ESPGPU_FMT_INDEX8:
    case ESPGPU_FMT_R8: return 1;
    default: return 0;
    }
}

bool gp_tex_upload(gpu_core_t *c, const espgpu_upload_texture_t *u, uint8_t flags,
                   const uint8_t *data, uint32_t len)
{
    const unsigned bpp = fmt_bpp(u->format);
    if (bpp == 0 || u->width == 0 || u->height == 0 || u->id == 0 ||
        u->width > c->cfg.max_texture_dim || u->height > c->cfg.max_texture_dim) {
        gp_log(GP_LOG_WARN, "texture %u: bad upload %ux%u fmt %u", (unsigned)u->id, u->width,
               u->height, u->format);
        return false;
    }
    const uint32_t pixels = (uint32_t)u->width * u->height;
    const uint32_t expected = pixels * bpp;
    if (u->total_len != expected || (u->offset % bpp) != 0 || u->offset > expected ||
        len > expected - u->offset) {
        gp_log(GP_LOG_WARN, "texture %u: bad range off=%u len=%u total=%u", (unsigned)u->id,
               (unsigned)u->offset, (unsigned)len, (unsigned)u->total_len);
        return false;
    }

    /* 存储格式：RGBA4444 展开成 RGB565 + alpha 平面 */
    const uint8_t sfmt = (u->format == ESPGPU_FMT_RGBA4444) ? ESPGPU_FMT_RGB565 : u->format;
    const bool want_alpha = u->format == ESPGPU_FMT_RGBA4444;

    if (u->offset != 0) {
        /* 续传片必须与首片的存储形态一致：否则 RGB565 首片 + RGBA4444 续片会写空的 alpha 平面 */
        gp_tex_t *e = gp_tex_find(&c->tc, u->id);
        if (e == NULL || e->total_len != expected || e->w != u->width || e->h != u->height ||
            e->fmt != sfmt || (sfmt == ESPGPU_FMT_RGB565 ? e->px == NULL : e->idx == NULL) ||
            (want_alpha && e->alpha == NULL)) {
            gp_log(GP_LOG_WARN, "texture %u: continuation does not match first chunk",
                   (unsigned)u->id);
            return false;
        }
    }

    gp_tex_t *t = gp_tex_get(c, u->id);
    if (t == NULL) {
        return false;
    }
    gp_tex_before_modify(c, t);

    if (u->offset == 0) {
        /* 同尺寸同格式时复用存储（glTexSubImage 式的整张更新很常见） */
        const bool reuse = t->w == u->width && t->h == u->height && t->fmt == sfmt &&
                           (t->px != NULL || t->idx != NULL) && ((t->alpha != NULL) == want_alpha);
        if (!reuse) {
            uint16_t *keep_pal = t->pal;
            uint8_t *keep_pal_a = t->pal_a;
            const uint32_t pal_bytes = (keep_pal ? 512u : 0u) + (keep_pal_a ? 256u : 0u);
            t->pal = NULL;
            t->pal_a = NULL;
            tex_free_storage(&c->tc, t);
            /* 调色板与像素独立：先设调色板再传像素是合法顺序 */
            t->pal = keep_pal;
            t->pal_a = keep_pal_a;
            t->bytes = pal_bytes;
            c->tc.bytes += pal_bytes;

            t->w = u->width;
            t->h = u->height;
            t->fmt = sfmt;
            if (sfmt == ESPGPU_FMT_RGB565) {
                t->px = (uint16_t *)tex_alloc(c, (size_t)pixels * 2, t);
                if (t->px == NULL) goto oom;
                t->bytes += pixels * 2;
                if (want_alpha) {
                    t->alpha = (uint8_t *)tex_alloc(c, pixels, t);
                    if (t->alpha == NULL) goto oom;
                    t->bytes += pixels;
                }
            } else {
                t->idx = (uint8_t *)tex_alloc(c, pixels, t);
                if (t->idx == NULL) goto oom;
                t->bytes += pixels;
            }
        }
        t->priority = (flags & ESPGPU_TEXF_PRIORITY) ? 1 : 0;
        t->has_alpha = want_alpha || (t->fmt == ESPGPU_FMT_INDEX8 && t->pal_a != NULL);
        t->total_len = expected;
        t->received = 0;
    }

    const uint32_t first = u->offset / bpp;
    const uint32_t n = len / bpp;
    if (u->format == ESPGPU_FMT_RGBA4444) {
        for (uint32_t i = 0; i < n; i++) {
            uint16_t s;
            memcpy(&s, data + i * 2, 2);
            const uint32_t r = (s >> 12) & 0xF, g = (s >> 8) & 0xF, b = (s >> 4) & 0xF,
                           a = s & 0xF;
            t->px[first + i] = (uint16_t)(((r * 31 / 15) << 11) | ((g * 63 / 15) << 5) |
                                          (b * 31 / 15));
            t->alpha[first + i] = (uint8_t)(a * 17);
        }
    } else if (u->format == ESPGPU_FMT_RGB565) {
        memcpy(t->px + first, data, (size_t)n * 2);
    } else {
        memcpy(t->idx + first, data, n);
    }
    t->received += len;
    t->last_used = c->frame;
    return true;

oom:
    gp_log(GP_LOG_ERROR, "texture %u: out of memory (%ux%u)", (unsigned)u->id, u->width,
           u->height);
    tex_destroy(c, t);
    gp_send_event(c, ESPGPU_EVT_TEXTURE_EVICTED, 0, u->id);
    return false;
}

void gp_tex_palette(gpu_core_t *c, const espgpu_tex_palette_t *p, const uint8_t *data,
                    uint32_t len)
{
    if (p->first >= 256 || p->count == 0) {
        return;
    }
    uint32_t count = p->count;
    if (p->first + count > 256) {
        count = 256u - p->first;
    }
    if (len < count * 3) {
        return;
    }
    gp_tex_t *t = gp_tex_get(c, p->id);
    if (t == NULL) {
        return;
    }
    gp_tex_before_modify(c, t);
    if (t->pal == NULL) {
        t->pal = (uint16_t *)tex_alloc(c, 512, t);
        if (t->pal == NULL) {
            return;
        }
        t->bytes += 512;
        for (int i = 0; i < 256; i++) {
            t->pal[i] = gp_pack565((uint32_t)i, (uint32_t)i, (uint32_t)i);
        }
    }
    memcpy(t->pal + p->first, data, count * 2);

    const uint8_t *alpha = data + count * 2;
    bool opaque = true;
    for (uint32_t i = 0; i < count; i++) {
        if (alpha[i] != 255) {
            opaque = false;
            break;
        }
    }
    if (!opaque && t->pal_a == NULL) {
        t->pal_a = (uint8_t *)tex_alloc(c, 256, t);
        if (t->pal_a) {
            t->bytes += 256;
            memset(t->pal_a, 255, 256);
        }
    }
    if (t->pal_a) {
        memcpy(t->pal_a + p->first, alpha, count);
    }
    t->has_alpha = t->pal_a != NULL;
}

/* ── 采样 ─────────────────────────────────────────────────────────── */

/* 实现在 core_internal.h（光栅器内联展开）；这里是给片元 VM 等调用方的出口 */
GP_HOT uint32_t gp_sample(const gp_sampler_t *s, float u, float v)
{
    const gp_tex_t *t = s->tex;
    if (t == NULL || (t->px == NULL && t->idx == NULL)) {
        return 0xFFFFFFFFu;
    }
    return s->filter == ESPGPU_FILTER_LINEAR ? gp_sample_linear(s, u, v)
                                             : gp_sample_nearest(s, u, v);
}
