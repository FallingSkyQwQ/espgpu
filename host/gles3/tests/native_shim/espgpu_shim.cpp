/*
 * espgpu_shim.cpp — espgpu.dll 的本机替身（只给 gles_e2e 用）
 *
 * 只实现 libGLESv2 用到的那些 API：命令按 host/src/draw3d.cpp 的编码拼好后直接交给
 * 固件核心 gpu_core_exec；绘制只走单条命令的快速路径（测试里的绘制都很小）。
 */
#include "espgpu/espgpu.h"
#include "core.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

struct espgpu_device {
    gpu_core_t *core = nullptr;
    std::vector<uint16_t> frame;
    int fw = 0, fh = 0;
    uint32_t frame_id = 0;
    uint8_t layout_index_type = 1;
    std::deque<espgpu_event_t> events;
    espgpu_device_info_t info{};
    uint32_t fence = 0;
    size_t bytes = 0; /* 下行命令字节数统计 */
};

static espgpu_device g_dev;

static void on_event(void *u, const espgpu_event_t *ev)
{
    auto *d = static_cast<espgpu_device *>(u);
    if (ev->kind == ESPGPU_EVT_FENCE) d->fence = ev->arg;
    d->events.push_back(*ev);
}
static void on_present(void *u, const uint16_t *px, uint16_t w, uint16_t h)
{
    auto *d = static_cast<espgpu_device *>(u);
    d->frame.assign(px, px + size_t(w) * h);
    d->fw = w;
    d->fh = h;
    d->frame_id++;
}

static void push(espgpu_device_t *d, uint8_t op, uint8_t flags, const void *body, uint32_t size)
{
    std::vector<uint8_t> b(sizeof(espgpu_cmd_t) + size);
    espgpu_cmd_t c{};
    c.op = op;
    c.flags = flags;
    c.size = static_cast<uint16_t>(size);
    std::memcpy(b.data(), &c, sizeof(c));
    if (size) std::memcpy(b.data() + sizeof(c), body, size);
    d->bytes += b.size();
    gpu_core_exec(d->core, b.data(), static_cast<uint32_t>(b.size()));
}

extern "C" {
size_t shim_bytes() { return g_dev.bytes; }
void shim_reset_bytes() { g_dev.bytes = 0; }

int espgpu_device_count(void) { return 1; }
espgpu_device_t *espgpu_open(int, char *, size_t)
{
    gpu_core_config_t cfg{};
    cfg.arena_bytes = 4u << 20;
    cfg.texture_budget = 4u << 20;
    cfg.buffer_budget = 1u << 20;
    cfg.max_texture_dim = 512;
    gpu_core_cb_t cb{on_event, on_present, &g_dev};
    g_dev.core = gpu_core_create(&cfg, &cb);
    g_dev.info.texture_cache_size = cfg.texture_budget;
    g_dev.info.max_texture_dim = 512;
    return &g_dev;
}
void espgpu_close(espgpu_device_t *d)
{
    if (d->core) gpu_core_destroy(d->core);
    d->core = nullptr;
}
const espgpu_device_info_t *espgpu_cached_info(espgpu_device_t *d) { return &d->info; }
int espgpu_set_mode(espgpu_device_t *d, const espgpu_mode_t *m, uint8_t, int)
{
    gpu_core_set_size(d->core, m->width, m->height);
    return 0;
}
int espgpu_set_codec(espgpu_device_t *, int, int) { return 0; }
uint8_t espgpu_preferred_codec(espgpu_device_t *) { return ESPGPU_CODEC_AUTO; }
void espgpu_cmd_discard(espgpu_device_t *) {}
int espgpu_cmd_pipeline(espgpu_device_t *d, const espgpu_pipeline_t *p, const espgpu_pipeline_fog_t *fog,
                        const espgpu_pipeline_ext_t *ext)
{
    uint8_t body[sizeof(*p) + sizeof(*fog) + sizeof(*ext)];
    uint32_t n = sizeof(*p);
    std::memcpy(body, p, n);
    espgpu_pipeline_fog_t f{0.0f, 1.0f, 1.0f};
    if (fog) f = *fog;
    std::memcpy(body + n, &f, sizeof(f));
    n += sizeof(f);
    if (ext) {
        std::memcpy(body + n, ext, sizeof(*ext));
        n += sizeof(*ext);
    }
    push(d, ESPGPU_OP_PIPELINE, 0, body, n);
    return 0;
}
int espgpu_cmd_matrix(espgpu_device_t *d, uint8_t slot, const float m[16])
{
    uint8_t body[sizeof(espgpu_set_matrix_t) + 64];
    espgpu_set_matrix_t h{};
    h.slot = slot;
    h.flags = ESPGPU_MATF_FLOAT;
    std::memcpy(body, &h, sizeof(h));
    std::memcpy(body + sizeof(h), m, 64);
    push(d, ESPGPU_OP_SET_MATRIX, 0, body, sizeof(body));
    return 0;
}
int espgpu_cmd_vertex_layout(espgpu_device_t *d, const espgpu_vertex_layout_t *l)
{
    d->layout_index_type = l->index_type == 2 ? 2 : 1;
    push(d, ESPGPU_OP_VERTEX_LAYOUT, 0, l, sizeof(*l));
    return 0;
}
int espgpu_cmd_draw(espgpu_device_t *d, uint8_t prim, const void *verts, uint32_t nv, uint32_t stride,
                    const void *idx, uint32_t ni, uint8_t itype)
{
    if (idx && itype != d->layout_index_type) return -1; /* 测试只走快速路径 */
    std::vector<uint8_t> body(sizeof(espgpu_draw_t) + size_t(nv) * stride + (idx ? size_t(ni) * (itype == 2 ? 4 : 2) : 0));
    espgpu_draw_t h{};
    h.prim = prim;
    h.source = ESPGPU_VSRC_INLINE;
    h.vertex_count = static_cast<uint16_t>(nv);
    h.index_count = static_cast<uint16_t>(idx ? ni : 0);
    std::memcpy(body.data(), &h, sizeof(h));
    std::memcpy(body.data() + sizeof(h), verts, size_t(nv) * stride);
    if (idx) std::memcpy(body.data() + sizeof(h) + size_t(nv) * stride, idx, size_t(ni) * (itype == 2 ? 4 : 2));
    push(d, ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, body.data(), static_cast<uint32_t>(body.size()));
    return 0;
}
int espgpu_cmd_bind_texture_uv(espgpu_device_t *d, uint8_t unit, uint32_t id, uint8_t filter, uint8_t wu, uint8_t wv)
{
    espgpu_bind_texture_t b{};
    b.unit = unit;
    b.texture = static_cast<uint16_t>(id);
    b.filter = filter;
    b.wrap = wu;
    b.pad = wv;
    push(d, ESPGPU_OP_BIND_TEXTURE, ESPGPU_BINDF_WRAP_V, &b, sizeof(b));
    return 0;
}
int espgpu_cmd_upload_texture_ex(espgpu_device_t *d, uint32_t id, uint16_t w, uint16_t h, uint8_t fmt, const void *px,
                                 uint8_t flags)
{
    const uint32_t bpp = fmt == ESPGPU_FMT_R8 || fmt == ESPGPU_FMT_INDEX8 ? 1 : 2;
    const uint32_t total = uint32_t(w) * h * bpp;
    const uint32_t frag = 8192;
    for (uint32_t off = 0; off < total; off += frag) {
        const uint32_t n = std::min(total - off, frag);
        std::vector<uint8_t> body(sizeof(espgpu_upload_texture_t) + n);
        espgpu_upload_texture_t u{};
        u.id = id;
        u.width = w;
        u.height = h;
        u.offset = off;
        u.total_len = total;
        u.format = fmt;
        std::memcpy(body.data(), &u, sizeof(u));
        std::memcpy(body.data() + sizeof(u), static_cast<const uint8_t *>(px) + off, n);
        push(d, ESPGPU_OP_UPLOAD_TEXTURE, flags, body.data(), static_cast<uint32_t>(body.size()));
    }
    return 0;
}
int espgpu_cmd_delete_texture(espgpu_device_t *d, uint32_t id)
{
    espgpu_delete_texture_t x{id};
    push(d, ESPGPU_OP_DELETE_TEXTURE, 0, &x, sizeof(x));
    return 0;
}
int espgpu_cmd_clear_ex(espgpu_device_t *d, uint8_t flags, uint16_t color, uint16_t depth)
{
    espgpu_clear_t c{};
    c.flags = flags;
    c.color = color;
    c.depth = depth;
    push(d, ESPGPU_OP_CLEAR, 0, &c, sizeof(c));
    return 0;
}
int espgpu_cmd_viewport(espgpu_device_t *d, int16_t x, int16_t y, int16_t w, int16_t h)
{
    espgpu_rect_t r{uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h)};
    push(d, ESPGPU_OP_VIEWPORT, 0, &r, sizeof(r));
    return 0;
}
int espgpu_cmd_scissor(espgpu_device_t *d, int16_t x, int16_t y, int16_t w, int16_t h)
{
    espgpu_rect_t r{uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h)};
    push(d, ESPGPU_OP_SCISSOR, 0, &r, sizeof(r));
    return 0;
}
int espgpu_cmd_depth_range(espgpu_device_t *d, float n, float f)
{
    espgpu_depth_range_t r{n, f};
    push(d, ESPGPU_OP_DEPTH_RANGE, 0, &r, sizeof(r));
    return 0;
}
int espgpu_cmd_color(espgpu_device_t *d, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    espgpu_color_t c{r, g, b, a};
    push(d, ESPGPU_OP_COLOR, 0, &c, sizeof(c));
    return 0;
}
int espgpu_cmd_load_program(espgpu_device_t *d, uint8_t id, uint8_t nvar, const espgpu_vm_insn_t *code, uint32_t n)
{
    std::vector<uint8_t> body(sizeof(espgpu_load_program_t) + n * sizeof(espgpu_vm_insn_t));
    espgpu_load_program_t h{};
    h.program_id = id;
    h.varying_count = nvar;
    h.insn_count = static_cast<uint16_t>(n);
    std::memcpy(body.data(), &h, sizeof(h));
    std::memcpy(body.data() + sizeof(h), code, n * sizeof(espgpu_vm_insn_t));
    push(d, ESPGPU_OP_LOAD_PROGRAM, 0, body.data(), static_cast<uint32_t>(body.size()));
    return 0;
}
int espgpu_cmd_set_uniforms(espgpu_device_t *d, uint8_t first, uint8_t count, const float *v)
{
    std::vector<uint8_t> body(sizeof(espgpu_set_uniforms_t) + count * 16u);
    espgpu_set_uniforms_t h{};
    h.first = first;
    h.count = count;
    std::memcpy(body.data(), &h, sizeof(h));
    std::memcpy(body.data() + sizeof(h), v, count * 16u);
    push(d, ESPGPU_OP_SET_UNIFORMS, 0, body.data(), static_cast<uint32_t>(body.size()));
    return 0;
}
int espgpu_cmd_fbo(espgpu_device_t *d, const espgpu_fbo_t *f)
{
    push(d, ESPGPU_OP_FBO, 0, f, sizeof(*f));
    return 0;
}
int espgpu_cmd_fence(espgpu_device_t *d, uint32_t v)
{
    espgpu_fence_t f{v};
    push(d, ESPGPU_OP_FENCE, 0, &f, sizeof(f));
    return 0;
}
int espgpu_cmd_flush(espgpu_device_t *, int) { return 0; }
int espgpu_wait_fence(espgpu_device_t *, uint32_t, int) { return 1; }
int espgpu_cmd_present(espgpu_device_t *d, int)
{
    push(d, ESPGPU_OP_PRESENT, 0, nullptr, 0);
    return 0;
}
uint32_t espgpu_frame_id(espgpu_device_t *d) { return d->frame_id; }
int espgpu_wait_frame(espgpu_device_t *d, uint32_t after, int) { return d->frame_id > after ? 1 : 0; }
int espgpu_blit_to_hdc(espgpu_device_t *, void *, int, int) { return 0; }
int espgpu_pop_event(espgpu_device_t *d, espgpu_event_t *ev)
{
    if (d->events.empty()) return 0;
    *ev = d->events.front();
    d->events.pop_front();
    return 1;
}
int espgpu_frame_snapshot_bgra(espgpu_device_t *d, uint8_t *dst, size_t cap, size_t, int *w, int *h, uint32_t *id)
{
    if (d->frame.empty() || cap < size_t(d->fw) * d->fh * 4) return -1;
    for (size_t i = 0; i < d->frame.size(); i++) {
        const uint16_t p = d->frame[i];
        dst[i * 4 + 0] = uint8_t(((p & 31) * 255 + 15) / 31);
        dst[i * 4 + 1] = uint8_t((((p >> 5) & 63) * 255 + 31) / 63);
        dst[i * 4 + 2] = uint8_t((((p >> 11) & 31) * 255 + 15) / 31);
        dst[i * 4 + 3] = 255;
    }
    *w = d->fw;
    *h = d->fh;
    if (id) *id = d->frame_id;
    return 0;
}
}
