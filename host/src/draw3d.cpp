/*
 * draw3d.cpp — M3-M6 的通用 3D 命令（GL 1.2 / GLES 3.0 前端共用）
 *
 * 和 draw.cpp 一样只往主机侧命令缓冲里追加，PRESENT / flush 时才真正发出。
 * 大块数据（纹理、顶点、缓冲）在这里拆成单条命令放得下的分片。
 */

#include "internal.h"

#include <algorithm>
#include <cstring>

using namespace espgpu;

namespace {

unsigned fmt_bpp(uint8_t fmt)
{
    switch (fmt) {
    case ESPGPU_FMT_RGB565:
    case ESPGPU_FMT_RGBA4444: return 2;
    case ESPGPU_FMT_INDEX8:
    case ESPGPU_FMT_R8: return 1;
    default: return 0;
    }
}

uint32_t read_index(const void *idx, uint8_t type, uint32_t i)
{
    if (type == 2) {
        uint32_t v;
        memcpy(&v, static_cast<const uint8_t *>(idx) + i * 4u, 4);
        return v;
    }
    uint16_t v;
    memcpy(&v, static_cast<const uint8_t *>(idx) + i * 2u, 2);
    return v;
}

/** 把任意拓扑展开成基础列表图元的顶点序列（strip / fan / loop → 列表），用于拆批 */
uint8_t expand_prim(uint8_t prim, uint32_t count, std::vector<uint32_t> &seq,
                    const void *idx, uint8_t itype)
{
    auto at = [&](uint32_t k) { return idx ? read_index(idx, itype, k) : k; };
    seq.clear();
    switch (prim) {
    case ESPGPU_PRIM_TRIANGLE_STRIP:
        for (uint32_t k = 0; k + 2 < count; k++) {
            /* 奇数个三角形交换前两个顶点保持绕序；第三个仍是激发顶点 */
            if (k & 1) {
                seq.insert(seq.end(), {at(k + 1), at(k), at(k + 2)});
            } else {
                seq.insert(seq.end(), {at(k), at(k + 1), at(k + 2)});
            }
        }
        return ESPGPU_PRIM_TRIANGLES;
    case ESPGPU_PRIM_TRIANGLE_FAN:
        for (uint32_t k = 1; k + 1 < count; k++) {
            seq.insert(seq.end(), {at(0), at(k), at(k + 1)});
        }
        return ESPGPU_PRIM_TRIANGLES;
    case ESPGPU_PRIM_LINE_STRIP:
    case ESPGPU_PRIM_LINE_LOOP:
        for (uint32_t k = 0; k + 1 < count; k++) {
            seq.insert(seq.end(), {at(k), at(k + 1)});
        }
        if (prim == ESPGPU_PRIM_LINE_LOOP && count > 2) {
            seq.insert(seq.end(), {at(count - 1), at(0)});
        }
        return ESPGPU_PRIM_LINES;
    default:
        for (uint32_t k = 0; k < count; k++) {
            seq.push_back(at(k));
        }
        return prim;
    }
}

uint32_t prim_group(uint8_t prim)
{
    switch (prim) {
    case ESPGPU_PRIM_TRIANGLES: return 3;
    case ESPGPU_PRIM_QUADS: return 4;
    case ESPGPU_PRIM_LINES: return 2;
    default: return 1;
    }
}

} // namespace

extern "C" {

int espgpu_cmd_raw(espgpu_device_t *dev, uint8_t op, uint8_t flags, const void *body,
                   uint32_t size)
{
    if (dev == nullptr || size > kCmdMaxBody) {
        return -1;
    }
    cmd_push(dev, op, flags, body, size);
    return 0;
}

int espgpu_cmd_pipeline(espgpu_device_t *dev, const espgpu_pipeline_t *p,
                        const espgpu_pipeline_fog_t *fog, const espgpu_pipeline_ext_t *ext)
{
    if (dev == nullptr || p == nullptr) {
        return -1;
    }
    uint8_t body[sizeof(espgpu_pipeline_t) + sizeof(espgpu_pipeline_fog_t) +
                 sizeof(espgpu_pipeline_ext_t)];
    uint32_t n = sizeof(espgpu_pipeline_t);
    memcpy(body, p, n);
    if (fog || ext) {
        /* ext 依赖 fog 段的位置：只给 ext 时 fog 段填默认值，并且不置 FOG 掩码就不会生效 */
        espgpu_pipeline_fog_t f{};
        f.fog_start = 0.0f;
        f.fog_end = 1.0f;
        f.fog_density = 1.0f;
        if (fog) {
            f = *fog;
        }
        memcpy(body + n, &f, sizeof(f));
        n += sizeof(f);
    }
    if (ext) {
        memcpy(body + n, ext, sizeof(*ext));
        n += sizeof(*ext);
    }
    cmd_push(dev, ESPGPU_OP_PIPELINE, 0, body, n);
    return 0;
}

int espgpu_cmd_matrix(espgpu_device_t *dev, uint8_t slot, const float m[16])
{
    if (dev == nullptr || slot >= ESPGPU_MATRIX_COUNT) {
        return -1;
    }
    uint8_t body[sizeof(espgpu_set_matrix_t) + 64];
    espgpu_set_matrix_t h{};
    h.slot = slot;
    if (m == nullptr) {
        h.flags = ESPGPU_MATF_IDENTITY;
        memcpy(body, &h, sizeof(h));
        cmd_push(dev, ESPGPU_OP_SET_MATRIX, 0, body, sizeof(h));
        return 0;
    }
    h.flags = ESPGPU_MATF_FLOAT;
    memcpy(body, &h, sizeof(h));
    memcpy(body + sizeof(h), m, 64);
    cmd_push(dev, ESPGPU_OP_SET_MATRIX, 0, body, sizeof(body));
    return 0;
}

int espgpu_cmd_vertex_layout(espgpu_device_t *dev, const espgpu_vertex_layout_t *l)
{
    if (dev == nullptr || l == nullptr) {
        return -1;
    }
    dev->layout_index_type = l->index_type == 2 ? 2 : 1;
    dev->layout_stride = l->stride;
    cmd_push(dev, ESPGPU_OP_VERTEX_LAYOUT, 0, l, sizeof(*l));
    return 0;
}

int espgpu_cmd_draw(espgpu_device_t *dev, uint8_t prim, const void *verts, uint32_t vertex_count,
                    uint32_t stride, const void *indices, uint32_t index_count,
                    uint8_t index_type)
{
    if (dev == nullptr || verts == nullptr || stride == 0 || vertex_count == 0) {
        return -1;
    }
    if (stride != dev->layout_stride) {
        log_msg(LOG_WARN, "draw stride %u != layout stride %u", stride, dev->layout_stride);
    }
    index_type = index_type == 2 ? 2 : 1;
    const uint32_t isz = index_type == 2 ? 4u : 2u;
    const auto *vb = static_cast<const uint8_t *>(verts);
    if (indices != nullptr && index_count == 0) {
        return 0; /* glDrawElements(count = 0)：什么都不画（发 index_count=0 会被设备当成非索引绘制） */
    }

    /* 快速路径：一条命令放得下且索引类型与布局一致 */
    const uint64_t one = sizeof(espgpu_draw_t) + uint64_t(vertex_count) * stride +
                         (indices ? uint64_t(index_count) * isz : 0);
    if (one <= kCmdMaxBody && vertex_count <= 0xFFFF && index_count <= 0xFFFF &&
        (indices == nullptr || index_type == dev->layout_index_type)) {
        std::vector<uint8_t> body(static_cast<size_t>(one));
        espgpu_draw_t d{};
        d.prim = prim;
        d.source = ESPGPU_VSRC_INLINE;
        d.vertex_count = static_cast<uint16_t>(vertex_count);
        d.index_count = static_cast<uint16_t>(indices ? index_count : 0);
        memcpy(body.data(), &d, sizeof(d));
        memcpy(body.data() + sizeof(d), vb, size_t(vertex_count) * stride);
        if (indices) {
            memcpy(body.data() + sizeof(d) + size_t(vertex_count) * stride, indices,
                   size_t(index_count) * isz);
        }
        cmd_push(dev, ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, body.data(),
                 static_cast<uint32_t>(body.size()));
        return 0;
    }

    /* 拆批：先展开成列表图元（strip / fan / loop → 列表），每批按图元边界切 */
    std::vector<uint32_t> seq;
    const uint8_t base = expand_prim(prim, indices ? index_count : vertex_count, seq, indices,
                                     index_type);
    const uint32_t group = prim_group(base);
    const size_t budget = kCmdMaxBody - sizeof(espgpu_draw_t);

    if (indices == nullptr && base == prim) {
        /* 非索引的列表图元：直接按顶点切，没有任何重复数据 */
        const uint32_t per_batch_verts =
            static_cast<uint32_t>(budget / stride) / group * group;
        if (per_batch_verts == 0) {
            return -1;
        }
        std::vector<uint8_t> body;
        uint32_t done = 0;
        while (done + group <= vertex_count) {
            uint32_t n = std::min(vertex_count - done, per_batch_verts);
            n -= n % group;
            espgpu_draw_t d{};
            d.prim = base;
            d.source = ESPGPU_VSRC_INLINE;
            d.vertex_count = static_cast<uint16_t>(n);
            body.resize(sizeof(d) + size_t(n) * stride);
            memcpy(body.data(), &d, sizeof(d));
            memcpy(body.data() + sizeof(d), vb + size_t(done) * stride, size_t(n) * stride);
            cmd_push(dev, ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, body.data(),
                     static_cast<uint32_t>(body.size()));
            done += n;
        }
        return 0;
    }

    /* 索引绘制 / 展开过的拓扑：每批重新编号成局部索引，只带这一批用到的顶点。
     * 以前是整批去索引（每个三角形 3 个完整顶点），共享顶点的网格要多发 3–5 倍字节，
     * 而 USB 正是瓶颈。输出索引类型跟随当前 VERTEX_LAYOUT（设备按它解释索引）。 */
    const size_t oisz = dev->layout_index_type == 2 ? 4u : 2u;
    if (size_t(group) * (stride + oisz) > budget) {
        return -1;
    }
    std::vector<uint32_t> stamp(vertex_count, 0); /* stamp[v] == epoch → v 已在本批 */
    std::vector<uint32_t> local(vertex_count, 0);
    uint32_t epoch = 1;
    std::vector<uint8_t> vbuf;
    std::vector<uint8_t> ibuf;
    std::vector<uint8_t> body;
    uint32_t nverts = 0;
    uint32_t nidx = 0;

    auto flush_batch = [&] {
        if (nidx == 0) {
            return;
        }
        espgpu_draw_t d{};
        d.prim = base;
        d.source = ESPGPU_VSRC_INLINE;
        d.vertex_count = static_cast<uint16_t>(nverts);
        d.index_count = static_cast<uint16_t>(nidx);
        body.resize(sizeof(d) + vbuf.size() + ibuf.size());
        memcpy(body.data(), &d, sizeof(d));
        memcpy(body.data() + sizeof(d), vbuf.data(), vbuf.size());
        memcpy(body.data() + sizeof(d) + vbuf.size(), ibuf.data(), ibuf.size());
        cmd_push(dev, ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, body.data(),
                 static_cast<uint32_t>(body.size()));
        vbuf.clear();
        ibuf.clear();
        nverts = 0;
        nidx = 0;
        epoch++;
    };

    for (size_t k = 0; k + group <= seq.size(); k += group) {
        /* 设备对越界索引的语义是丢掉整个图元（tnl.c 的 VALID），这里保持一致 */
        bool valid = true;
        for (uint32_t j = 0; j < group; j++) {
            valid = valid && seq[k + j] < vertex_count;
        }
        if (!valid) {
            continue;
        }
        /* 这个图元会新增多少个顶点（组内重复的只算一次） */
        uint32_t fresh = 0;
        for (uint32_t j = 0; j < group; j++) {
            const uint32_t v = seq[k + j];
            bool dup = stamp[v] == epoch;
            for (uint32_t q = 0; q < j && !dup; q++) {
                dup = seq[k + q] == v;
            }
            fresh += dup ? 0u : 1u;
        }
        if (nidx != 0 && vbuf.size() + ibuf.size() + size_t(fresh) * stride +
                                 size_t(group) * oisz > budget) {
            flush_batch();
        }
        for (uint32_t j = 0; j < group; j++) {
            const uint32_t v = seq[k + j];
            if (stamp[v] != epoch) {
                stamp[v] = epoch;
                local[v] = nverts++;
                vbuf.insert(vbuf.end(), vb + size_t(v) * stride, vb + size_t(v + 1) * stride);
            }
            const uint32_t li = local[v];
            ibuf.insert(ibuf.end(), reinterpret_cast<const uint8_t *>(&li),
                        reinterpret_cast<const uint8_t *>(&li) + oisz); /* 小端：低 2 字节即 u16 */
            nidx++;
        }
    }
    flush_batch();
    return 0;
}

int espgpu_cmd_draw_buffer(espgpu_device_t *dev, uint8_t prim, uint32_t buffer_id,
                           uint32_t base_vertex, uint32_t vertex_count, uint32_t index_offset,
                           uint32_t index_count)
{
    if (dev == nullptr || buffer_id == 0 || vertex_count > 0xFFFF || index_count > 0xFFFF ||
        base_vertex > 0xFFFF) {
        return -1;
    }
    espgpu_draw_t d{};
    d.prim = prim;
    d.source = ESPGPU_VSRC_BUFFER;
    d.vertex_count = static_cast<uint16_t>(vertex_count);
    d.index_count = static_cast<uint16_t>(index_count);
    d.base_vertex = static_cast<uint16_t>(base_vertex);
    d.buffer_id = buffer_id;
    d.index_offset = index_offset;
    cmd_push(dev, ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, &d, sizeof(d));
    return 0;
}

int espgpu_cmd_bind_texture_unit(espgpu_device_t *dev, uint8_t unit, uint32_t id, uint8_t filter,
                                 uint8_t wrap)
{
    if (dev == nullptr || unit >= ESPGPU_MAX_TEXTURE_UNITS || id > 0xFFFF) {
        return -1;
    }
    espgpu_bind_texture_t b{};
    b.unit = unit;
    b.texture = static_cast<uint16_t>(id);
    b.filter = filter;
    b.wrap = wrap;
    cmd_push(dev, ESPGPU_OP_BIND_TEXTURE, 0, &b, sizeof(b));
    return 0;
}

int espgpu_cmd_bind_texture_uv(espgpu_device_t *dev, uint8_t unit, uint32_t id, uint8_t filter,
                               uint8_t wrap_u, uint8_t wrap_v)
{
    if (dev == nullptr || unit >= ESPGPU_MAX_TEXTURE_UNITS || id > 0xFFFF) {
        return -1;
    }
    espgpu_bind_texture_t b{};
    b.unit = unit;
    b.texture = static_cast<uint16_t>(id);
    b.filter = filter;
    b.wrap = wrap_u;
    b.pad = wrap_v;
    cmd_push(dev, ESPGPU_OP_BIND_TEXTURE, ESPGPU_BINDF_WRAP_V, &b, sizeof(b));
    return 0;
}

int espgpu_cmd_upload_texture_ex(espgpu_device_t *dev, uint32_t id, uint16_t width,
                                 uint16_t height, uint8_t format, const void *pixels,
                                 uint8_t flags)
{
    const unsigned bpp = fmt_bpp(format);
    if (dev == nullptr || pixels == nullptr || width == 0 || height == 0 || bpp == 0) {
        return -1;
    }
    if (uint64_t(width) * height * bpp > 0xFFFFFFFFu) {
        return -1; /* total_len 是 u32 */
    }
    const uint32_t total = static_cast<uint32_t>(width) * height * bpp;
    const uint32_t max_frag =
        static_cast<uint32_t>(kCmdMaxBody - sizeof(espgpu_upload_texture_t)) / bpp * bpp;
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
        cmd_push(dev, ESPGPU_OP_UPLOAD_TEXTURE, flags, body.data(),
                 static_cast<uint32_t>(body.size()));
        off += n;
    }
    return 0;
}

int espgpu_cmd_tex_palette(espgpu_device_t *dev, uint32_t id, uint16_t first, uint16_t count,
                           const uint16_t *rgb565, const uint8_t *alpha)
{
    if (dev == nullptr || rgb565 == nullptr || count == 0 || first + count > 256) {
        return -1;
    }
    std::vector<uint8_t> body(sizeof(espgpu_tex_palette_t) + count * 3u);
    espgpu_tex_palette_t p{};
    p.id = id;
    p.first = first;
    p.count = count;
    memcpy(body.data(), &p, sizeof(p));
    memcpy(body.data() + sizeof(p), rgb565, count * 2u);
    if (alpha) {
        memcpy(body.data() + sizeof(p) + count * 2u, alpha, count);
    } else {
        memset(body.data() + sizeof(p) + count * 2u, 255, count);
    }
    cmd_push(dev, ESPGPU_OP_TEX_PALETTE, 0, body.data(), static_cast<uint32_t>(body.size()));
    return 0;
}

int espgpu_cmd_delete_texture(espgpu_device_t *dev, uint32_t id)
{
    if (dev == nullptr) {
        return -1;
    }
    espgpu_delete_texture_t d{id};
    cmd_push(dev, ESPGPU_OP_DELETE_TEXTURE, 0, &d, sizeof(d));
    return 0;
}

int espgpu_cmd_clear_ex(espgpu_device_t *dev, uint8_t flags, uint16_t color, uint16_t depth)
{
    if (dev == nullptr) {
        return -1;
    }
    espgpu_clear_t c{};
    c.flags = flags;
    c.color = color;
    c.depth = depth;
    cmd_push(dev, ESPGPU_OP_CLEAR, 0, &c, sizeof(c));
    return 0;
}

int espgpu_cmd_viewport(espgpu_device_t *dev, int16_t x, int16_t y, int16_t w, int16_t h)
{
    if (dev == nullptr) {
        return -1;
    }
    espgpu_rect_t r{static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(w),
                    static_cast<uint16_t>(h)};
    cmd_push(dev, ESPGPU_OP_VIEWPORT, 0, &r, sizeof(r));
    return 0;
}

int espgpu_cmd_depth_range(espgpu_device_t *dev, float znear, float zfar)
{
    if (dev == nullptr) {
        return -1;
    }
    espgpu_depth_range_t r{znear, zfar};
    cmd_push(dev, ESPGPU_OP_DEPTH_RANGE, 0, &r, sizeof(r));
    return 0;
}

int espgpu_cmd_color(espgpu_device_t *dev, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (dev == nullptr) {
        return -1;
    }
    espgpu_color_t c{r, g, b, a};
    cmd_push(dev, ESPGPU_OP_COLOR, 0, &c, sizeof(c));
    return 0;
}

int espgpu_cmd_lighting(espgpu_device_t *dev, bool enable, const float ambient[3],
                        const espgpu_light_t *lights, uint32_t count)
{
    if (dev == nullptr) {
        return -1;
    }
    if (count > ESPGPU_MAX_LIGHTS) {
        count = ESPGPU_MAX_LIGHTS;
    }
    uint8_t body[sizeof(espgpu_lighting_t) + ESPGPU_MAX_LIGHTS * sizeof(espgpu_light_t)];
    espgpu_lighting_t h{};
    h.enable = enable ? 1 : 0;
    h.count = static_cast<uint8_t>(lights ? count : 0);
    if (ambient) {
        h.ambient[0] = ambient[0];
        h.ambient[1] = ambient[1];
        h.ambient[2] = ambient[2];
    }
    memcpy(body, &h, sizeof(h));
    if (lights && count) {
        memcpy(body + sizeof(h), lights, count * sizeof(espgpu_light_t));
    }
    cmd_push(dev, ESPGPU_OP_LIGHTING, 0, body,
             static_cast<uint32_t>(sizeof(h) + h.count * sizeof(espgpu_light_t)));
    return 0;
}

int espgpu_cmd_load_program(espgpu_device_t *dev, uint8_t program_id, uint8_t varying_count,
                            const espgpu_vm_insn_t *code, uint32_t insn_count)
{
    if (dev == nullptr || code == nullptr || insn_count == 0 ||
        insn_count > ESPGPU_VM_MAX_INSNS || program_id >= ESPGPU_VM_MAX_PROGRAMS) {
        return -1;
    }
    std::vector<uint8_t> body(sizeof(espgpu_load_program_t) + insn_count * sizeof(espgpu_vm_insn_t));
    espgpu_load_program_t h{};
    h.program_id = program_id;
    h.varying_count = varying_count;
    h.insn_count = static_cast<uint16_t>(insn_count);
    memcpy(body.data(), &h, sizeof(h));
    memcpy(body.data() + sizeof(h), code, insn_count * sizeof(espgpu_vm_insn_t));
    cmd_push(dev, ESPGPU_OP_LOAD_PROGRAM, 0, body.data(), static_cast<uint32_t>(body.size()));
    return 0;
}

int espgpu_cmd_set_uniforms(espgpu_device_t *dev, uint8_t first, uint8_t count,
                            const float *vec4s)
{
    if (dev == nullptr || vec4s == nullptr || count == 0 || first + count > ESPGPU_VM_CONSTS) {
        return -1;
    }
    std::vector<uint8_t> body(sizeof(espgpu_set_uniforms_t) + count * 16u);
    espgpu_set_uniforms_t h{};
    h.first = first;
    h.count = count;
    memcpy(body.data(), &h, sizeof(h));
    memcpy(body.data() + sizeof(h), vec4s, count * 16u);
    cmd_push(dev, ESPGPU_OP_SET_UNIFORMS, 0, body.data(), static_cast<uint32_t>(body.size()));
    return 0;
}

int espgpu_cmd_fbo(espgpu_device_t *dev, const espgpu_fbo_t *f)
{
    if (dev == nullptr || f == nullptr) {
        return -1;
    }
    cmd_push(dev, ESPGPU_OP_FBO, 0, f, sizeof(*f));
    return 0;
}

int espgpu_cmd_fence(espgpu_device_t *dev, uint32_t value)
{
    if (dev == nullptr) {
        return -1;
    }
    espgpu_fence_t f{value};
    cmd_push(dev, ESPGPU_OP_FENCE, 0, &f, sizeof(f));
    return 0;
}

int espgpu_cmd_flush(espgpu_device_t *dev, int timeout_ms)
{
    if (dev == nullptr) {
        return -1;
    }
    return cmd_flush(dev, timeout_ms);
}

int espgpu_wait_fence(espgpu_device_t *dev, uint32_t value, int timeout_ms)
{
    if (dev == nullptr) {
        return -1;
    }
    ApiRef ref(dev);
    cmd_flush(dev, 500);
    std::unique_lock<std::mutex> lk(dev->evt_mu);
    bool ok = false;
    dev->evt_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms), [&] {
        ok = dev->have_fence && static_cast<int32_t>(dev->last_fence - value) >= 0;
        return ok || !dev->connected.load() || !dev->running.load();
    });
    if (ok) {
        return 0;
    }
    return (!dev->connected.load() || !dev->running.load()) ? -1 : -2;
}

int espgpu_wait_frame(espgpu_device_t *dev, uint32_t after_frame_id, int timeout_ms)
{
    if (dev == nullptr) {
        return -1;
    }
    ApiRef ref(dev);
    link_service_keyframe(dev);
    std::unique_lock<std::mutex> lk(dev->frame_mu);
    bool ok = false;
    dev->frame_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms), [&] {
        ok = dev->frame_id != 0 && static_cast<int32_t>(dev->frame_id - after_frame_id) > 0;
        return ok || !dev->connected.load() || !dev->running.load();
    });
    if (ok) {
        return 1;
    }
    return (!dev->connected.load() || !dev->running.load()) ? -1 : 0;
}

int espgpu_buffer_upload(espgpu_device_t *dev, uint32_t id, const void *data, uint32_t len)
{
    if (dev == nullptr || id == 0 || (data == nullptr && len != 0)) {
        return -1;
    }
    /* DATA 不走命令缓冲：先把已攒的命令发出去，保证顺序 */
    if (cmd_flush(dev, 500) != 0 && !dev->connected.load()) {
        return -1;
    }
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
        h.kind = ESPGPU_DATA_BUFFER;
        body.resize(sizeof(h) + n);
        memcpy(body.data(), &h, sizeof(h));
        if (n) {
            memcpy(body.data() + sizeof(h), src + off, n);
        }
        if (link_send_chunk(dev, ESPGPU_CHUNK_DATA, 0, body.data(),
                            static_cast<uint32_t>(body.size()), 1000) != 0) {
            return -1;
        }
        off += n;
    } while (off < len);
    return 0;
}

int espgpu_buffer_delete(espgpu_device_t *dev, uint32_t id)
{
    if (dev == nullptr || id == 0) {
        return -1;
    }
    cmd_flush(dev, 500);
    espgpu_data_hdr_t h{};
    h.buffer_id = id;
    h.kind = ESPGPU_DATA_BUFFER;
    h.flags = ESPGPU_DATAF_DELETE;
    return link_send_chunk(dev, ESPGPU_CHUNK_DATA, 0, &h, sizeof(h), 500);
}

void espgpu_set_compression(espgpu_device_t *dev, bool enable)
{
    if (dev) {
        dev->compress = enable;
    }
}

int espgpu_pop_event(espgpu_device_t *dev, espgpu_event_t *ev)
{
    if (dev == nullptr || ev == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(dev->evt_mu);
    if (dev->events.empty()) {
        return 0;
    }
    *ev = dev->events.front();
    dev->events.pop_front();
    return 1;
}

} // extern "C"
