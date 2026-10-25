/*
 * fake_espgpu.cpp — espgpu.dll 的本地替身：命令体格式与 host/src/draw*.cpp 一致，
 * 按真实规则攒 chunk（24 KB 阈值）、LZ 压缩计字节，并把命令流交给 firmware/core 真正渲染。
 */
#include "espgpu/espgpu.h"
#include "espgpu_lz.h"
extern "C" {
#include "core.h"
}
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

struct espgpu_device {
    int x;
};
static espgpu_device g_d;

constexpr size_t kCmdFlushThreshold = 24 * 1024;
constexpr size_t kCmdMaxBody = kCmdFlushThreshold - 64;

struct FakeStats {
    uint64_t raw = 0, wire = 0, chunks = 0;
    uint64_t op_bytes[32] = {}, op_count[32] = {};
    uint64_t data_bytes = 0;
    uint64_t draw_verts = 0;
};
FakeStats g_fs;
bool g_fake_render = true;
static std::vector<uint8_t> g_cmd;
static uint16_t g_layout_stride = 0;
static uint8_t g_layout_itype = 1;
static gpu_core_t *g_core = nullptr;
static std::deque<espgpu_event_t> g_events;
std::vector<uint16_t> g_fake_frame;
int g_fake_fw = 0, g_fake_fh = 0;
static uint32_t g_frame_id = 0;
static std::vector<uint32_t> g_lz_tab; static std::vector<uint8_t> g_lz_buf;

static void on_event(void *, const espgpu_event_t *ev) { g_events.push_back(*ev); }
static void on_present(void *, const uint16_t *px, uint16_t w, uint16_t h)
{
    g_fake_frame.assign(px, px + size_t(w) * h);
    g_fake_fw = w;
    g_fake_fh = h;
    g_frame_id++;
}

static void send_chunk(uint8_t type, const uint8_t *p, size_t n)
{
    size_t wire = n;
    if (n >= 256) {
        g_lz_tab.resize(size_t(1) << ESPGPU_LZ_HASH_BITS);
        g_lz_buf.resize(4 + espgpu_lz_bound(n));
        const size_t c = espgpu_lz_encode(p, n, g_lz_buf.data() + 4, g_lz_buf.size() - 4,
                                          g_lz_tab.data());
        if (c > 0 && c + 4 < n) {
            wire = c + 4;
        }
    }
    g_fs.raw += 12 + n;
    g_fs.wire += 12 + wire;
    g_fs.chunks++;
    if (g_core && g_fake_render) {
        if (type == ESPGPU_CHUNK_CMD) {
            gpu_core_exec(g_core, p, static_cast<uint32_t>(n));
        } else {
            gpu_core_data(g_core, p, static_cast<uint32_t>(n));
        }
    }
}

extern "C" bool g_fake_connected_c();
static int cmd_flush()
{
    if (g_cmd.empty()) return 0;
    if (!g_fake_connected_c()) { g_cmd.clear(); return -1; }
    send_chunk(ESPGPU_CHUNK_CMD, g_cmd.data(), g_cmd.size());
    g_cmd.clear();
    return 0;
}

static void cmd_push(uint8_t op, uint8_t flags, const void *body, uint32_t size)
{
    if (size > kCmdMaxBody) {
        fprintf(stderr, "body too large %u\n", size);
        return;
    }
    if (g_cmd.size() + 4 + size > kCmdFlushThreshold) cmd_flush();
    espgpu_cmd_t c{op, flags, static_cast<uint16_t>(size)};
    const uint8_t *q = reinterpret_cast<const uint8_t *>(&c);
    g_cmd.insert(g_cmd.end(), q, q + 4);
    static const int strip = getenv("FAKE_STRIP") ? atoi(getenv("FAKE_STRIP")) : -1;
    if (size && body) {
        g_cmd.insert(g_cmd.end(), static_cast<const uint8_t *>(body), static_cast<const uint8_t *>(body) + size);
        if (strip == op) {
            g_fake_render = false; /* 只统计 */
            std::fill(g_cmd.end() - size, g_cmd.end(), 0);
        }
    }
    g_fs.op_bytes[op & 31] += 4 + size;
    g_fs.op_count[op & 31]++;
}

static uint32_t read_index(const void *idx, uint8_t type, uint32_t i)
{
    if (type == 2) { uint32_t v; memcpy(&v, static_cast<const uint8_t *>(idx) + i * 4u, 4); return v; }
    uint16_t v; memcpy(&v, static_cast<const uint8_t *>(idx) + i * 2u, 2); return v;
}

static uint8_t expand_prim(uint8_t prim, uint32_t count, std::vector<uint32_t> &seq, const void *idx, uint8_t itype)
{
    auto at = [&](uint32_t k) { return idx ? read_index(idx, itype, k) : k; };
    seq.clear();
    switch (prim) {
    case ESPGPU_PRIM_TRIANGLE_STRIP:
        for (uint32_t k = 0; k + 2 < count; k++) {
            if (k & 1) seq.insert(seq.end(), {at(k + 1), at(k), at(k + 2)});
            else seq.insert(seq.end(), {at(k), at(k + 1), at(k + 2)});
        }
        return ESPGPU_PRIM_TRIANGLES;
    case ESPGPU_PRIM_TRIANGLE_FAN:
        for (uint32_t k = 1; k + 1 < count; k++) seq.insert(seq.end(), {at(0), at(k), at(k + 1)});
        return ESPGPU_PRIM_TRIANGLES;
    case ESPGPU_PRIM_LINE_STRIP:
    case ESPGPU_PRIM_LINE_LOOP:
        for (uint32_t k = 0; k + 1 < count; k++) seq.insert(seq.end(), {at(k), at(k + 1)});
        if (prim == ESPGPU_PRIM_LINE_LOOP && count > 2) seq.insert(seq.end(), {at(count - 1), at(0)});
        return ESPGPU_PRIM_LINES;
    default:
        for (uint32_t k = 0; k < count; k++) seq.push_back(at(k));
        return prim;
    }
}

static uint32_t prim_group(uint8_t prim)
{
    switch (prim) {
    case ESPGPU_PRIM_TRIANGLES: return 3;
    case ESPGPU_PRIM_QUADS: return 4;
    case ESPGPU_PRIM_LINES: return 2;
    default: return 1;
    }
}

int g_split_draws = 0;
bool g_fake_connected = true;
int g_fake_opens = 0;
extern "C" bool g_fake_connected_c() { return g_fake_connected; }

extern "C" {
int espgpu_device_count(void) { return 1; }
espgpu_device_t *espgpu_open(int, char *, size_t)
{
    g_fake_opens++;
    g_fake_connected = true;
    gpu_core_config_t cfg{};
    cfg.arena_bytes = 4u << 20;
    cfg.texture_budget = 4u << 20;
    cfg.buffer_budget = 4u << 20;
    cfg.max_texture_dim = 512;
    gpu_core_cb_t cb{};
    cb.send_event = on_event;
    cb.present = on_present;
    g_core = gpu_core_create(&cfg, &cb);
    return &g_d;
}
uint8_t espgpu_preferred_codec(espgpu_device_t *) { return ESPGPU_CODEC_AUTO; }
/* ESPGPU_GL_LOG 的 profile 用；替身没有设备统计 */
int espgpu_get_stats(espgpu_device_t *, espgpu_stats_t *st, int)
{
    if (st) memset(st, 0, sizeof(*st));
    return 0;
}
int espgpu_get_link_stats(espgpu_device_t *, espgpu_link_stats_t *ls)
{
    if (ls) memset(ls, 0, sizeof(*ls));
    return 0;
}

int espgpu_set_mode(espgpu_device_t *, const espgpu_mode_t *m, uint8_t, int)
{
    if (g_core) gpu_core_set_size(g_core, m->width, m->height);
    return 0;
}
const espgpu_device_info_t *espgpu_cached_info(espgpu_device_t *)
{
    static espgpu_device_info_t i{};
    i.max_texture_dim = 512;
    i.texture_cache_size = 4u << 20;
    return &i;
}
void espgpu_set_compression(espgpu_device_t *, bool) {}
uint32_t espgpu_frame_id(espgpu_device_t *) { return g_frame_id; }
bool espgpu_is_emulator(espgpu_device_t *) { return true; }
bool espgpu_is_connected(espgpu_device_t *) { return g_fake_connected; }
void espgpu_close(espgpu_device_t *)
{
    if (g_core) gpu_core_destroy(g_core);
    g_core = nullptr;
    g_cmd.clear();
}
int espgpu_pop_event(espgpu_device_t *, espgpu_event_t *ev)
{
    if (g_events.empty()) return 0;
    *ev = g_events.front();
    g_events.pop_front();
    return 1;
}
int espgpu_cmd_pipeline(espgpu_device_t *, const espgpu_pipeline_t *p, const espgpu_pipeline_fog_t *fog,
                        const espgpu_pipeline_ext_t *ext)
{
    uint8_t body[sizeof(espgpu_pipeline_t) + sizeof(espgpu_pipeline_fog_t) + sizeof(espgpu_pipeline_ext_t)];
    uint32_t n = sizeof(espgpu_pipeline_t);
    memcpy(body, p, n);
    if (fog || ext) {
        espgpu_pipeline_fog_t f{0.0f, 1.0f, 1.0f};
        if (fog) f = *fog;
        memcpy(body + n, &f, sizeof(f));
        n += sizeof(f);
    }
    if (ext) {
        memcpy(body + n, ext, sizeof(*ext));
        n += sizeof(*ext);
    }
    cmd_push(ESPGPU_OP_PIPELINE, 0, body, n);
    return 0;
}
int espgpu_cmd_matrix(espgpu_device_t *, uint8_t slot, const float m[16])
{
    uint8_t body[sizeof(espgpu_set_matrix_t) + 64];
    espgpu_set_matrix_t h{};
    h.slot = slot;
    if (m == nullptr) {
        h.flags = ESPGPU_MATF_IDENTITY;
        cmd_push(ESPGPU_OP_SET_MATRIX, 0, &h, sizeof(h));
        return 0;
    }
    h.flags = ESPGPU_MATF_FLOAT;
    memcpy(body, &h, sizeof(h));
    memcpy(body + sizeof(h), m, 64);
    cmd_push(ESPGPU_OP_SET_MATRIX, 0, body, sizeof(body));
    return 0;
}
int espgpu_cmd_vertex_layout(espgpu_device_t *, const espgpu_vertex_layout_t *l)
{
    g_layout_itype = l->index_type == 2 ? 2 : 1;
    g_layout_stride = l->stride;
    cmd_push(ESPGPU_OP_VERTEX_LAYOUT, 0, l, sizeof(*l));
    return 0;
}
int espgpu_cmd_color(espgpu_device_t *, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    espgpu_color_t c{r, g, b, a};
    cmd_push(ESPGPU_OP_COLOR, 0, &c, sizeof(c));
    return 0;
}
int espgpu_cmd_draw(espgpu_device_t *, uint8_t prim, const void *verts, uint32_t vertex_count, uint32_t stride,
                    const void *indices, uint32_t index_count, uint8_t index_type)
{
    if (verts == nullptr || stride == 0 || vertex_count == 0) return -1;
    if (stride != g_layout_stride) fprintf(stderr, "STRIDE MISMATCH %u %u\n", stride, g_layout_stride);
    g_fs.draw_verts += vertex_count;
    index_type = index_type == 2 ? 2 : 1;
    const uint32_t isz = index_type == 2 ? 4u : 2u;
    const auto *vb = static_cast<const uint8_t *>(verts);
    const uint64_t one = sizeof(espgpu_draw_t) + uint64_t(vertex_count) * stride + (indices ? uint64_t(index_count) * isz : 0);
    if (one <= kCmdMaxBody && vertex_count <= 0xFFFF && index_count <= 0xFFFF &&
        (indices == nullptr || index_type == g_layout_itype)) {
        std::vector<uint8_t> body(static_cast<size_t>(one));
        espgpu_draw_t d{};
        d.prim = prim;
        d.vertex_count = static_cast<uint16_t>(vertex_count);
        d.index_count = static_cast<uint16_t>(indices ? index_count : 0);
        memcpy(body.data(), &d, sizeof(d));
        memcpy(body.data() + sizeof(d), vb, size_t(vertex_count) * stride);
        if (indices) memcpy(body.data() + sizeof(d) + size_t(vertex_count) * stride, indices, size_t(index_count) * isz);
        cmd_push(ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, body.data(), static_cast<uint32_t>(body.size()));
        return 0;
    }
    g_split_draws++;
    std::vector<uint32_t> seq;
    const uint8_t base = expand_prim(prim, indices ? index_count : vertex_count, seq, indices, index_type);
    const uint32_t group = prim_group(base);
    const uint32_t per = static_cast<uint32_t>((kCmdMaxBody - sizeof(espgpu_draw_t)) / stride) / group * group;
    std::vector<uint8_t> body;
    size_t done = 0;
    while (done + group <= seq.size()) {
        size_t n = std::min<size_t>(seq.size() - done, per);
        n -= n % group;
        espgpu_draw_t d{};
        d.prim = base;
        d.vertex_count = static_cast<uint16_t>(n);
        body.resize(sizeof(d) + n * stride);
        memcpy(body.data(), &d, sizeof(d));
        for (size_t k = 0; k < n; k++) {
            const uint32_t vi = seq[done + k];
            if (vi < vertex_count) memcpy(body.data() + sizeof(d) + k * stride, vb + size_t(vi) * stride, stride);
            else memset(body.data() + sizeof(d) + k * stride, 0, stride);
        }
        cmd_push(ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, body.data(), static_cast<uint32_t>(body.size()));
        done += n;
    }
    return 0;
}
int espgpu_cmd_draw_buffer(espgpu_device_t *, uint8_t prim, uint32_t buffer_id, uint32_t base_vertex,
                           uint32_t vertex_count, uint32_t index_offset, uint32_t index_count)
{
    if (buffer_id == 0 || vertex_count > 0xFFFF || index_count > 0xFFFF || base_vertex > 0xFFFF) return -1;
    espgpu_draw_t d{};
    d.prim = prim;
    d.source = ESPGPU_VSRC_BUFFER;
    d.vertex_count = static_cast<uint16_t>(vertex_count);
    d.index_count = static_cast<uint16_t>(index_count);
    d.base_vertex = static_cast<uint16_t>(base_vertex);
    d.buffer_id = buffer_id;
    d.index_offset = index_offset;
    cmd_push(ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, &d, sizeof(d));
    return 0;
}
int espgpu_buffer_upload(espgpu_device_t *, uint32_t id, const void *data, uint32_t len)
{
    cmd_flush();
    const uint32_t frag = static_cast<uint32_t>(kCmdMaxBody - sizeof(espgpu_data_hdr_t));
    const auto *src = static_cast<const uint8_t *>(data);
    std::vector<uint8_t> body;
    uint32_t off = 0;
    do {
        const uint32_t n = std::min(len - off, frag);
        espgpu_data_hdr_t h{};
        h.buffer_id = id;
        h.offset = off;
        h.total_len = len;
        body.resize(sizeof(h) + n);
        memcpy(body.data(), &h, sizeof(h));
        if (n) memcpy(body.data() + sizeof(h), src + off, n);
        send_chunk(ESPGPU_CHUNK_DATA, body.data(), body.size());
        g_fs.data_bytes += body.size();
        off += n;
    } while (off < len);
    return 0;
}
int espgpu_buffer_delete(espgpu_device_t *, uint32_t id)
{
    cmd_flush();
    espgpu_data_hdr_t h{};
    h.buffer_id = id;
    h.flags = ESPGPU_DATAF_DELETE;
    send_chunk(ESPGPU_CHUNK_DATA, reinterpret_cast<uint8_t *>(&h), sizeof(h));
    return 0;
}
int espgpu_cmd_bind_texture_uv(espgpu_device_t *, uint8_t unit, uint32_t id, uint8_t filter, uint8_t wu, uint8_t wv)
{
    espgpu_bind_texture_t b{};
    b.unit = unit;
    b.texture = static_cast<uint16_t>(id);
    b.filter = filter;
    b.wrap = wu;
    b.pad = wv;
    cmd_push(ESPGPU_OP_BIND_TEXTURE, ESPGPU_BINDF_WRAP_V, &b, sizeof(b));
    return 0;
}
int espgpu_cmd_upload_texture_ex(espgpu_device_t *, uint32_t id, uint16_t width, uint16_t height, uint8_t format,
                                 const void *pixels, uint8_t flags)
{
    const unsigned bpp = (format == ESPGPU_FMT_RGB565 || format == ESPGPU_FMT_RGBA4444) ? 2 : 1;
    const uint32_t total = static_cast<uint32_t>(width) * height * bpp;
    const uint32_t max_frag = static_cast<uint32_t>(kCmdMaxBody - sizeof(espgpu_upload_texture_t)) / bpp * bpp;
    const auto *src = static_cast<const uint8_t *>(pixels);
    std::vector<uint8_t> body;
    uint32_t off = 0;
    while (off < total) {
        const uint32_t n = std::min(total - off, max_frag);
        espgpu_upload_texture_t u{};
        u.id = id;
        u.width = width;
        u.height = height;
        u.offset = off;
        u.total_len = total;
        u.format = format;
        body.resize(sizeof(u) + n);
        memcpy(body.data(), &u, sizeof(u));
        memcpy(body.data() + sizeof(u), src + off, n);
        cmd_push(ESPGPU_OP_UPLOAD_TEXTURE, flags, body.data(), static_cast<uint32_t>(body.size()));
        off += n;
    }
    return 0;
}
int espgpu_cmd_tex_palette(espgpu_device_t *, uint32_t id, uint16_t first, uint16_t count, const uint16_t *rgb565,
                           const uint8_t *alpha)
{
    std::vector<uint8_t> body(sizeof(espgpu_tex_palette_t) + count * 3u);
    espgpu_tex_palette_t p{id, first, count};
    memcpy(body.data(), &p, sizeof(p));
    memcpy(body.data() + sizeof(p), rgb565, count * 2u);
    if (alpha) memcpy(body.data() + sizeof(p) + count * 2u, alpha, count);
    else memset(body.data() + sizeof(p) + count * 2u, 255, count);
    cmd_push(ESPGPU_OP_TEX_PALETTE, 0, body.data(), static_cast<uint32_t>(body.size()));
    return 0;
}
int espgpu_cmd_delete_texture(espgpu_device_t *, uint32_t id)
{
    espgpu_delete_texture_t d{id};
    cmd_push(ESPGPU_OP_DELETE_TEXTURE, 0, &d, sizeof(d));
    return 0;
}
int espgpu_cmd_clear_ex(espgpu_device_t *, uint8_t flags, uint16_t color, uint16_t depth)
{
    espgpu_clear_t c{};
    c.flags = flags;
    c.color = color;
    c.depth = depth;
    cmd_push(ESPGPU_OP_CLEAR, 0, &c, sizeof(c));
    return 0;
}
int espgpu_cmd_viewport(espgpu_device_t *, int16_t x, int16_t y, int16_t w, int16_t h)
{
    espgpu_rect_t r{static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
    cmd_push(ESPGPU_OP_VIEWPORT, 0, &r, sizeof(r));
    return 0;
}
int espgpu_cmd_scissor(espgpu_device_t *, int16_t x, int16_t y, int16_t w, int16_t h)
{
    espgpu_rect_t r{static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
    cmd_push(ESPGPU_OP_SCISSOR, 0, &r, sizeof(r));
    return 0;
}
int espgpu_cmd_depth_range(espgpu_device_t *, float n, float f)
{
    espgpu_depth_range_t r{n, f};
    cmd_push(ESPGPU_OP_DEPTH_RANGE, 0, &r, sizeof(r));
    return 0;
}
int espgpu_cmd_lighting(espgpu_device_t *, bool enable, const float *ambient, const espgpu_light_t *lights, uint32_t count)
{
    if (count > ESPGPU_MAX_LIGHTS) count = ESPGPU_MAX_LIGHTS;
    uint8_t body[sizeof(espgpu_lighting_t) + ESPGPU_MAX_LIGHTS * sizeof(espgpu_light_t)];
    espgpu_lighting_t h{};
    h.enable = enable ? 1 : 0;
    h.count = static_cast<uint8_t>(lights ? count : 0);
    if (ambient) { h.ambient[0] = ambient[0]; h.ambient[1] = ambient[1]; h.ambient[2] = ambient[2]; }
    memcpy(body, &h, sizeof(h));
    if (lights && count) memcpy(body + sizeof(h), lights, count * sizeof(espgpu_light_t));
    cmd_push(ESPGPU_OP_LIGHTING, 0, body, static_cast<uint32_t>(sizeof(h) + h.count * sizeof(espgpu_light_t)));
    return 0;
}
int espgpu_cmd_fbo(espgpu_device_t *, const espgpu_fbo_t *f)
{
    cmd_push(ESPGPU_OP_FBO, 0, f, sizeof(*f));
    return 0;
}
int espgpu_cmd_fence(espgpu_device_t *, uint32_t v)
{
    espgpu_fence_t f{v};
    cmd_push(ESPGPU_OP_FENCE, 0, &f, sizeof(f));
    return 0;
}
int espgpu_cmd_flush(espgpu_device_t *, int) { return cmd_flush(); }
int espgpu_wait_fence(espgpu_device_t *, uint32_t, int) { cmd_flush(); return 0; }
int espgpu_wait_fence_frame(espgpu_device_t *, uint32_t, int) { cmd_flush(); return 0; }
int espgpu_wait_frame(espgpu_device_t *, uint32_t after, int) { return g_frame_id > after ? 1 : 0; }
int espgpu_cmd_present(espgpu_device_t *, int)
{
    cmd_push(ESPGPU_OP_PRESENT, 0, nullptr, 0);
    return cmd_flush();
}
int espgpu_blit_to_hdc(espgpu_device_t *, void *, int, int) { return 0; }
int espgpu_frame_snapshot_bgra(espgpu_device_t *, uint8_t *dst, size_t cap, size_t, int *ow, int *oh, uint32_t *fid)
{
    if (g_fake_frame.empty() || cap < g_fake_frame.size() * 4) return -1;
    for (size_t i = 0; i < g_fake_frame.size(); i++) {
        const uint16_t v = g_fake_frame[i];
        dst[i * 4 + 0] = static_cast<uint8_t>((v & 31) * 255 / 31);
        dst[i * 4 + 1] = static_cast<uint8_t>(((v >> 5) & 63) * 255 / 63);
        dst[i * 4 + 2] = static_cast<uint8_t>((v >> 11) * 255 / 31);
        dst[i * 4 + 3] = 255;
    }
    *ow = g_fake_fw;
    *oh = g_fake_fh;
    *fid = g_frame_id;
    return 0;
}
} // extern "C"

void fake_reset_stats() { g_fs = FakeStats(); g_split_draws = 0; }
void fake_write_bmp(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f || g_fake_frame.empty()) { if (f) fclose(f); return; }
    const int w = g_fake_fw, h = g_fake_fh, row = w * 3, pad = (4 - row % 4) % 4;
    const uint32_t size = 54 + uint32_t(row + pad) * h;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            const uint16_t v = g_fake_frame[size_t(y) * w + x];
            const uint8_t bgr[3] = {uint8_t((v & 31) << 3), uint8_t(((v >> 5) & 63) << 2), uint8_t((v >> 11) << 3)};
            fwrite(bgr, 1, 3, f);
        }
        static const uint8_t z[3] = {};
        fwrite(z, 1, pad, f);
    }
    fclose(f);
}
const char *fake_op_name(int op)
{
    static const char *n[32] = {"nop", "PIPELINE", "MATRIX", "LAYOUT", "BIND", "UPLOAD", "DRAW", "CLEAR", "VIEWPORT", "SCISSOR", "clut", "FBO", "PRESENT", "q", "FENCE", "", "p2d", "PALETTE", "DELTEX", "COLOR", "LIGHTING", "", "", "DEPTHRANGE"};
    return n[op & 31] ? n[op & 31] : "?";
}
