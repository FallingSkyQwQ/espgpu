/*
 * gl_draw.cpp — 绘制路径与设备同步
 *
 * 每次 draw：
 *   1. 处理设备事件（纹理被 LRU 换出 → 标记需要重传）
 *   2. 渲染目标 / 视口 / 裁剪矩形与 GL 状态对齐
 *   3. 取顶点属性；顶点着色器命中 T&L 就把属性原样发给设备并下发 MVP，
 *      否则在主机上逐顶点解释执行，发裁剪坐标 + varying，MVP 置单位阵
 *   4. 片元着色器：内置模式 → PIPELINE.shade_mode；否则 PROGRAM + VM 字节码 + 常量
 *   5. 纹理按需上传、绑定到设备单元
 *   6. 顶点打包成 F32 交错数组，espgpu_cmd_draw
 *
 * 所有设备状态都有镜像，只发变化的部分。
 */

#include "context.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace glsl = espgpu::glsl;

namespace gles {

/* ── 设备 ─────────────────────────────────────────────────────────── */

namespace {
Device g_device;
std::mutex g_device_mu;
int g_device_refs = 0;
} // namespace

Device &device() { return g_device; }

bool device_open()
{
    std::lock_guard<std::mutex> lk(g_device_mu);
    if (g_device_refs++ > 0) return g_device.ok;

    Device &d = g_device;
    d = Device{};
    const char *res = std::getenv("ESPGPU_RES");
    if (res) {
        int w = 0, h = 0;
        if (std::sscanf(res, "%dx%d", &w, &h) == 2 && w >= 32 && h >= 32 && w <= 800 && h <= 600) {
            d.width = w;
            d.height = h;
        }
    }
    char err[256] = {0};
    if (espgpu_device_count() <= 0) {
        OutputDebugStringA("[espgpu-gles] no ESP32-S3 GPU found (set ESPGPU_EMULATOR=1 for the software device)\n");
        std::fputs("[espgpu-gles] no ESP32-S3 GPU found (set ESPGPU_EMULATOR=1 for the software device)\n", stderr);
        return false;
    }
    d.dev = espgpu_open(0, err, sizeof(err));
    if (!d.dev) {
        std::fprintf(stderr, "[espgpu-gles] open failed: %s\n", err);
        return false;
    }
    if (const espgpu_device_info_t *info = espgpu_cached_info(d.dev)) {
        if (info->texture_cache_size) d.tex_budget = info->texture_cache_size;
        if (info->max_texture_dim) d.max_tex_dim = info->max_texture_dim;
    }
    espgpu_mode_t m{};
    m.width = static_cast<uint16_t>(d.width);
    m.height = static_cast<uint16_t>(d.height);
    m.format = ESPGPU_FMT_RGB565;
    m.codec = espgpu_preferred_codec(d.dev); /* AUTO：静止无损、动起来有损块编码 */
    m.max_fps = 30;
    m.vsync = 1;
    espgpu_set_mode(d.dev, &m, ESPGPU_PATTERN_DOOMISH, 1500);
    espgpu_set_codec(d.dev, m.codec, 500);
    if (const char *sync = std::getenv("ESPGPU_GLES_SYNC")) d.sync_swap = std::atoi(sync) != 0;
    d.ok = true;
    d.invalidate();
    return true;
}

void device_close()
{
    std::lock_guard<std::mutex> lk(g_device_mu);
    if (g_device_refs <= 0 || --g_device_refs > 0) return;
    if (g_device.dev) espgpu_close(g_device.dev);
    g_device = Device{};
}

/* ── 小工具 ───────────────────────────────────────────────────────── */

namespace {

uint16_t to565(float r, float g, float b)
{
    auto q = [](float v, int maxv) {
        v = std::min(std::max(v, 0.0f), 1.0f);
        return static_cast<uint32_t>(v * maxv + 0.5f);
    };
    return static_cast<uint16_t>((q(r, 31) << 11) | (q(g, 63) << 5) | q(b, 31));
}

uint8_t to8(float v)
{
    v = std::min(std::max(v, 0.0f), 1.0f);
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

uint8_t blend_factor(GLenum f)
{
    switch (f) {
    case GL_ZERO: return ESPGPU_BLEND_ZERO;
    case GL_ONE: return ESPGPU_BLEND_ONE;
    case GL_SRC_COLOR: return ESPGPU_BLEND_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return ESPGPU_BLEND_ONE_MINUS_SRC_COLOR;
    case GL_DST_COLOR: return ESPGPU_BLEND_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return ESPGPU_BLEND_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA: return ESPGPU_BLEND_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return ESPGPU_BLEND_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return ESPGPU_BLEND_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return ESPGPU_BLEND_ONE_MINUS_DST_ALPHA;
    default:
        warn_once("constant-color / saturate blend factors (mapped to ONE)");
        return ESPGPU_BLEND_ONE;
    }
}

uint8_t blend_op(GLenum e)
{
    switch (e) {
    case GL_FUNC_SUBTRACT: return ESPGPU_BLEND_OP_SUB;
    case GL_FUNC_REVERSE_SUBTRACT: return ESPGPU_BLEND_OP_REV_SUB;
    case GL_MIN: return ESPGPU_BLEND_OP_MIN;
    case GL_MAX: return ESPGPU_BLEND_OP_MAX;
    default: return ESPGPU_BLEND_OP_ADD;
    }
}

uint8_t filter_of(const Texture &t)
{
    return (t.mag_filter == GL_LINEAR || t.min_filter == GL_LINEAR || t.min_filter == GL_LINEAR_MIPMAP_LINEAR ||
            t.min_filter == GL_LINEAR_MIPMAP_NEAREST)
               ? ESPGPU_FILTER_LINEAR
               : ESPGPU_FILTER_NEAREST;
}

uint8_t wrap_mode(GLenum w)
{
    switch (w) {
    case GL_CLAMP_TO_EDGE: return ESPGPU_WRAP_CLAMP;
    case GL_MIRRORED_REPEAT: return ESPGPU_WRAP_MIRROR;
    default: return ESPGPU_WRAP_REPEAT;
    }
}

constexpr uint32_t kRenderbufferIdBase = 0xC000u;

/* ── 渲染目标 ── */

struct Target {
    uint32_t tex = 0; /* 0 = 屏幕 */
    int w = 0, h = 0;
    bool ok = true;
};

Target desired_target(Context *c)
{
    Device &d = device();
    Target t;
    if (!c->draw_fbo) {
        t.w = d.width;
        t.h = d.height;
        return t;
    }
    const Framebuffer &f = c->fbos[c->draw_fbo];
    if (f.color_tex) {
        auto it = c->sh->textures.find(f.color_tex);
        if (it == c->sh->textures.end() || it->second.width <= 0) {
            t.ok = false;
            return t;
        }
        t.tex = f.color_tex;
        t.w = it->second.width;
        t.h = it->second.height;
    } else if (f.color_rb) {
        auto it = c->sh->renderbuffers.find(f.color_rb);
        if (it == c->sh->renderbuffers.end() || it->second.width <= 0) {
            t.ok = false;
            return t;
        }
        t.tex = kRenderbufferIdBase + (f.color_rb & 0x3FFF);
        t.w = it->second.width;
        t.h = it->second.height;
    } else {
        t.ok = false;
    }
    return t;
}

/** 把 GL 的 (x,y,w,h)（左下原点、窗口像素）换成设备矩形 */
espgpu_rect_t map_rect(Context *c, const Target &t, const GLint r[4])
{
    Device &d = device();
    double x0 = r[0], y0 = r[1], x1 = static_cast<double>(r[0]) + r[2], y1 = static_cast<double>(r[1]) + r[3];
    if (t.tex == 0) {
        const double sx = static_cast<double>(d.width) / std::max(1, c->win_w);
        const double sy = static_cast<double>(d.height) / std::max(1, c->win_h);
        x0 *= sx;
        x1 *= sx;
        y0 *= sy;
        y1 *= sy;
        /* 屏幕：左上原点 */
        const double ty0 = d.height - y1, ty1 = d.height - y0;
        y0 = ty0;
        y1 = ty1;
    }
    auto clampi = [](double v) { return static_cast<int>(std::lround(std::min(std::max(v, -16384.0), 16384.0))); };
    espgpu_rect_t o;
    const int ix0 = clampi(x0), iy0 = clampi(y0), ix1 = clampi(x1), iy1 = clampi(y1);
    o.x = static_cast<uint16_t>(static_cast<int16_t>(ix0));
    o.y = static_cast<uint16_t>(static_cast<int16_t>(iy0));
    o.w = static_cast<uint16_t>(std::max(0, ix1 - ix0));
    o.h = static_cast<uint16_t>(std::max(0, iy1 - iy0));
    return o;
}

/** 渲染目标 + 视口 + 裁剪矩形 + 深度范围对齐。返回 false = 目标不完整 */
bool sync_target(Context *c)
{
    Device &d = device();
    const Target t = desired_target(c);
    if (!t.ok) {
        set_error(GL_INVALID_FRAMEBUFFER_OPERATION);
        return false;
    }
    if (!d.target_valid || d.target_tex != t.tex || d.target_w != t.w || d.target_h != t.h) {
        espgpu_fbo_t f{};
        if (t.tex == 0) {
            f.action = ESPGPU_FBO_BIND_DEFAULT;
        } else {
            f.action = ESPGPU_FBO_BIND_TEXTURE;
            f.texture_id = t.tex;
            f.width = static_cast<uint16_t>(t.w);
            f.height = static_cast<uint16_t>(t.h);
            if (t.tex < kRenderbufferIdBase) {
                Texture &tx = c->sh->textures[t.tex];
                /* 设备会按这个尺寸（重新）分配；主机母本之后作废 */
                if (!tx.resident) d.resident_bytes += static_cast<uint32_t>(t.w * t.h * 2);
                tx.device_only = true;
                tx.resident = true;
                tx.dirty = false;
                tx.dev_bytes = static_cast<uint32_t>(t.w * t.h * 2);
                tx.last_use = d.frame_counter;
            }
        }
        espgpu_cmd_fbo(d.dev, &f);
        d.target_valid = true;
        d.target_tex = t.tex;
        d.target_w = t.w;
        d.target_h = t.h;
        d.vp_valid = d.sc_valid = false;
        /* 绑定到别的单元上的同一张纹理：采样结果未定义，不处理 */
    }
    GLint vp[4] = {c->viewport[0], c->viewport[1], c->viewport[2], c->viewport[3]};
    if (!c->viewport_set) {
        vp[0] = vp[1] = 0;
        vp[2] = t.tex ? t.w : c->win_w;
        vp[3] = t.tex ? t.h : c->win_h;
    }
    const espgpu_rect_t v = map_rect(c, t, vp);
    if (!d.vp_valid || std::memcmp(&v, &d.viewport, sizeof(v)) != 0) {
        espgpu_cmd_viewport(d.dev, static_cast<int16_t>(v.x), static_cast<int16_t>(v.y), static_cast<int16_t>(v.w),
                            static_cast<int16_t>(v.h));
        d.viewport = v;
        d.vp_valid = true;
    }
    espgpu_rect_t s;
    if (c->scissor_test) {
        s = map_rect(c, t, c->scissor);
    } else {
        s.x = s.y = 0;
        s.w = static_cast<uint16_t>(t.w);
        s.h = static_cast<uint16_t>(t.h);
    }
    if (!d.sc_valid || std::memcmp(&s, &d.scissor, sizeof(s)) != 0) {
        espgpu_cmd_scissor(d.dev, static_cast<int16_t>(s.x), static_cast<int16_t>(s.y), static_cast<int16_t>(s.w),
                           static_cast<int16_t>(s.h));
        d.scissor = s;
        d.sc_valid = true;
    }
    if (!d.dr_valid || d.depth_near != c->depth_near || d.depth_far != c->depth_far) {
        espgpu_cmd_depth_range(d.dev, c->depth_near, c->depth_far);
        d.depth_near = c->depth_near;
        d.depth_far = c->depth_far;
        d.dr_valid = true;
    }
    return true;
}

/* ── 事件 / 纹理 ── */

void drain_events(Context *c)
{
    Device &d = device();
    espgpu_event_t ev;
    while (espgpu_pop_event(d.dev, &ev) == 1) {
        if (ev.kind == ESPGPU_EVT_TEXTURE_EVICTED) {
            auto it = c->sh->textures.find(ev.arg);
            if (it != c->sh->textures.end() && it->second.resident) {
                it->second.resident = false;
                d.resident_bytes -= std::min(d.resident_bytes, it->second.dev_bytes);
            }
            for (auto &u : d.units) {
                if (u.id == ev.arg) u.valid = false;
            }
        }
    }
}

/** 主机侧 LRU 镜像：上传前按预算主动换出最久未用的纹理，避免设备自己换出 */
void make_room(Context *c, uint32_t need, GLuint keep)
{
    Device &d = device();
    const uint32_t budget = d.tex_budget - d.tex_budget / 8;
    while (d.resident_bytes + need > budget) {
        GLuint victim = 0;
        uint64_t oldest = UINT64_MAX;
        for (auto &kv : c->sh->textures) {
            const Texture &t = kv.second;
            if (!t.resident || kv.first == keep || t.device_only || t.last_use >= d.frame_counter) continue;
            if (t.last_use < oldest) {
                oldest = t.last_use;
                victim = kv.first;
            }
        }
        if (!victim) break;
        Texture &t = c->sh->textures[victim];
        espgpu_cmd_delete_texture(d.dev, victim);
        t.resident = false;
        d.resident_bytes -= std::min(d.resident_bytes, t.dev_bytes);
        for (auto &u : d.units) {
            if (u.id == victim) u.valid = false;
        }
    }
}

void upload_texture(Context *c, GLuint id, Texture &t)
{
    Device &d = device();
    if (t.width <= 0 || t.height <= 0 || t.rgba.empty()) return;

    /* 超过设备上限就最近邻缩小 */
    int w = t.width, h = t.height;
    while (static_cast<uint32_t>(w) > d.max_tex_dim) w = (w + 1) / 2;
    while (static_cast<uint32_t>(h) > d.max_tex_dim) h = (h + 1) / 2;
    if (w != t.width || h != t.height) warn_once("textures larger than the device limit (downscaled)");

    bool opaque = true;
    if (t.has_alpha) {
        for (size_t i = 3; i < t.rgba.size(); i += 4) {
            if (t.rgba[i] != 255) {
                opaque = false;
                break;
            }
        }
    }
    uint8_t fmt;
    int bpp;
    if (t.luminance) {
        fmt = ESPGPU_FMT_R8;
        bpp = 1;
    } else if (opaque) {
        fmt = ESPGPU_FMT_RGB565;
        bpp = 2;
    } else {
        fmt = ESPGPU_FMT_RGBA4444;
        bpp = 2;
    }
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * bpp);
    for (int y = 0; y < h; y++) {
        const int sy = static_cast<int>(static_cast<int64_t>(y) * t.height / h);
        for (int x = 0; x < w; x++) {
            const int sx = static_cast<int>(static_cast<int64_t>(x) * t.width / w);
            const uint8_t *s = &t.rgba[(static_cast<size_t>(sy) * t.width + sx) * 4];
            uint8_t *o = &px[(static_cast<size_t>(y) * w + x) * bpp];
            if (fmt == ESPGPU_FMT_R8) {
                o[0] = s[0];
            } else {
                uint16_t v;
                if (fmt == ESPGPU_FMT_RGB565) {
                    v = static_cast<uint16_t>(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
                } else {
                    v = static_cast<uint16_t>(((s[0] >> 4) << 12) | ((s[1] >> 4) << 8) | ((s[2] >> 4) << 4) | (s[3] >> 4));
                }
                std::memcpy(o, &v, 2);
            }
        }
    }
    const uint32_t bytes = static_cast<uint32_t>(px.size());
    if (t.resident) d.resident_bytes -= std::min(d.resident_bytes, t.dev_bytes);
    t.resident = false;
    make_room(c, bytes, id);
    espgpu_cmd_upload_texture_ex(d.dev, id, static_cast<uint16_t>(w), static_cast<uint16_t>(h), fmt, px.data(), 0);
    t.resident = true;
    t.dirty = false;
    t.dev_bytes = bytes;
    d.resident_bytes += bytes;
    for (auto &u : d.units) {
        if (u.id == id) u.valid = false; /* 重新上传后重新绑定，保证采样状态生效 */
    }
}

void bind_unit(int unit, uint32_t id, uint8_t filter, uint8_t wrap_s, uint8_t wrap_t)
{
    Device &d = device();
    Device::UnitBind &u = d.units[unit];
    if (u.valid && u.id == id && u.filter == filter && u.wrap == wrap_s && u.wrap_t == wrap_t) return;
    espgpu_cmd_bind_texture_uv(d.dev, static_cast<uint8_t>(unit), id, filter, wrap_s, wrap_t);
    u.valid = true;
    u.id = id;
    u.filter = filter;
    u.wrap = wrap_s;
    u.wrap_t = wrap_t;
}

/* ── 顶点属性读取 ── */

float read_comp(const uint8_t *p, GLenum type, bool normalized, int i)
{
    switch (type) {
    case GL_FLOAT: {
        float v;
        std::memcpy(&v, p + i * 4, 4);
        return v;
    }
    case GL_HALF_FLOAT:
    case 0x8D61: {
        uint16_t h;
        std::memcpy(&h, p + i * 2, 2);
        const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
        float v = e == 0 ? std::ldexp(static_cast<float>(m), -24)
                         : std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
        return s ? -v : v;
    }
    case GL_FIXED: {
        int32_t v;
        std::memcpy(&v, p + i * 4, 4);
        return static_cast<float>(v) / 65536.0f;
    }
    case GL_BYTE: {
        const float v = static_cast<int8_t>(p[i]);
        return normalized ? std::max(v / 127.0f, -1.0f) : v;
    }
    case GL_UNSIGNED_BYTE: return normalized ? p[i] / 255.0f : static_cast<float>(p[i]);
    case GL_SHORT: {
        int16_t v;
        std::memcpy(&v, p + i * 2, 2);
        return normalized ? std::max(v / 32767.0f, -1.0f) : static_cast<float>(v);
    }
    case GL_UNSIGNED_SHORT: {
        uint16_t v;
        std::memcpy(&v, p + i * 2, 2);
        return normalized ? v / 65535.0f : static_cast<float>(v);
    }
    case GL_INT: {
        int32_t v;
        std::memcpy(&v, p + i * 4, 4);
        return normalized ? std::max(static_cast<float>(v / 2147483647.0), -1.0f) : static_cast<float>(v);
    }
    case GL_UNSIGNED_INT: {
        uint32_t v;
        std::memcpy(&v, p + i * 4, 4);
        return normalized ? static_cast<float>(v / 4294967295.0) : static_cast<float>(v);
    }
    default: return 0;
    }
}

int type_size(GLenum t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: case 0x8D61: return 2;
    default: return 4;
    }
}

struct Fetch {
    const uint8_t *base = nullptr; /* nullptr = 用通用属性常量 */
    size_t limit = 0;              /* 缓冲大小（客户端数组为 SIZE_MAX） */
    int stride = 0;
    int size = 4;
    GLenum type = GL_FLOAT;
    bool normalized = false;
    float konst[4] = {0, 0, 0, 1};

    void get(uint32_t i, float out[4]) const
    {
        if (!base) {
            std::memcpy(out, konst, sizeof(konst));
            return;
        }
        out[0] = out[1] = out[2] = 0;
        out[3] = 1;
        const size_t off = static_cast<size_t>(i) * stride;
        if (off + static_cast<size_t>(type_size(type)) * size > limit) return;
        const uint8_t *p = base + off;
        for (int k = 0; k < size; k++) out[k] = read_comp(p, type, normalized, k);
    }
};

/* ── 程序 / uniform ── */

int ensure_program(const Program &p)
{
    Device &d = device();
    int slot = -1;
    for (int i = 0; i < static_cast<int>(ESPGPU_VM_MAX_PROGRAMS); i++) {
        if (d.prog_slots[i].prog == &p && d.prog_slots[i].serial == p.serial) slot = i;
    }
    if (slot < 0) {
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < static_cast<int>(ESPGPU_VM_MAX_PROGRAMS); i++) {
            if (d.prog_slots[i].last_use < oldest) {
                oldest = d.prog_slots[i].last_use;
                slot = i;
            }
        }
        const auto &fp = p.fsc->frag;
        espgpu_cmd_load_program(d.dev, static_cast<uint8_t>(slot), fp.varying_count, fp.code.data(),
                                static_cast<uint32_t>(fp.code.size()));
        d.prog_slots[slot].prog = &p;
        d.prog_slots[slot].serial = p.serial;
    }
    d.prog_slots[slot].last_use = d.frame_counter;
    return slot;
}

/** FS 叶子 → 程序 uniform */
const glsl::ProgramUniform *fs_uniform(const Program &p, int leaf)
{
    for (const auto &u : p.link.uniforms) {
        if (u.fs_leaf == leaf) return &u;
    }
    return nullptr;
}

/** 片元常量表 c[0 .. const_slots)：uniform 存储按 FS local 槽排好，常量 / 主机预计算由编译器填 */
int frag_consts(const Program &p, float (*c)[4])
{
    const auto &fs = *p.fsc;
    const auto &fp = fs.frag;
    std::vector<std::array<float, 4>> un(static_cast<size_t>(std::max(1, fs.uniform_slot_count)), {0, 0, 0, 0});
    for (size_t li = 0; li < fs.uniforms.size(); li++) {
        const auto &leaf = fs.uniforms[li];
        if (leaf.is_sampler || leaf.local_slot < 0) continue;
        const glsl::ProgramUniform *u = fs_uniform(p, static_cast<int>(li));
        if (!u) continue;
        const int n = leaf.array_size * leaf.columns;
        for (int k = 0; k < n && leaf.local_slot + k < fs.uniform_slot_count; k++) {
            un[static_cast<size_t>(leaf.local_slot + k)] = p.storage[static_cast<size_t>(u->storage + k)];
        }
    }
    const int count = std::min<int>(fp.const_slots, static_cast<int>(ESPGPU_VM_CONSTS));
    if (count <= 0) return 0;
    glsl::fragment_consts(fs, reinterpret_cast<const float(*)[4]>(un.data()), c);
    return count;
}

/** 常量寄存器只下发和设备镜像不同的那一段 */
void upload_consts(const float (*c)[4], int count)
{
    Device &d = device();
    if (count <= 0) return;
    int first = -1, last = -1;
    for (int i = 0; i < count; i++) {
        if (i >= d.consts_count || std::memcmp(c[i], d.consts[i], 16) != 0) {
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first < 0) return;
    espgpu_cmd_set_uniforms(d.dev, static_cast<uint8_t>(first), static_cast<uint8_t>(last - first + 1), &c[first][0]);
    std::memcpy(d.consts[first], c[first], static_cast<size_t>(last - first + 1) * 16);
    /* 镜像只在 [0, consts_count) 连续有效 */
    if (first <= std::max(0, d.consts_count)) d.consts_count = std::max(d.consts_count, last + 1);
}

/** 程序 uniform 的第 slot 槽（按 VS local 槽取） */
void vs_uniform_slots(const Program &p, std::vector<std::array<float, 4>> &out)
{
    const auto &vs = *p.vsc;
    out.assign(static_cast<size_t>(std::max(1, vs.uniform_slot_count)), {0, 0, 0, 0});
    for (const auto &u : p.link.uniforms) {
        if (u.vs_leaf < 0) continue;
        const auto &leaf = vs.uniforms[static_cast<size_t>(u.vs_leaf)];
        if (leaf.is_sampler || leaf.local_slot < 0) continue;
        const int n = leaf.array_size * leaf.columns;
        for (int k = 0; k < n; k++) out[static_cast<size_t>(leaf.local_slot + k)] = p.storage[static_cast<size_t>(u.storage + k)];
    }
}

/** GL 列主序 mat4 相乘：r = a * b */
void mat_mul(const float *a, const float *b, float *r)
{
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
            t[c * 4 + rr] = s;
        }
    std::memcpy(r, t, sizeof(t));
}

void send_mvp(const float *row_major)
{
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!row_major) row_major = kIdentity;
    Device &d = device();
    if (d.mvp_valid && std::memcmp(d.mvp, row_major, sizeof(d.mvp)) == 0) return;
    espgpu_cmd_matrix(d.dev, ESPGPU_MATRIX_MVP, row_major);
    std::memcpy(d.mvp, row_major, sizeof(d.mvp));
    d.mvp_valid = true;
}

uint8_t prim_of(GLenum mode)
{
    switch (mode) {
    case GL_POINTS: return ESPGPU_PRIM_POINTS;
    case GL_LINES: return ESPGPU_PRIM_LINES;
    case GL_LINE_STRIP: return ESPGPU_PRIM_LINE_STRIP;
    case GL_LINE_LOOP: return ESPGPU_PRIM_LINE_LOOP;
    case GL_TRIANGLES: return ESPGPU_PRIM_TRIANGLES;
    case GL_TRIANGLE_STRIP: return ESPGPU_PRIM_TRIANGLE_STRIP;
    case GL_TRIANGLE_FAN: return ESPGPU_PRIM_TRIANGLE_FAN;
    default: return 0xFF;
    }
}

void begin_frame_work(Context *c)
{
    drain_events(c);
}

} // namespace

/* ════════════════════════════════════════════════════════════════════
 * 绘制
 * ════════════════════════════════════════════════════════════════════ */

void draw(GLenum mode, GLint first, GLsizei count, GLenum itype, const void *indices, bool indexed)
{
    Context *c = current();
    if (!c) return;
    Device &d = device();
    const uint8_t prim = prim_of(mode);
    if (prim == 0xFF) return set_error(GL_INVALID_ENUM);
    if (!c->program) return set_error(GL_INVALID_OPERATION);
    auto pit = c->sh->programs.find(c->program);
    if (pit == c->sh->programs.end() || !pit->second->linked) return set_error(GL_INVALID_OPERATION);
    Program &p = *pit->second;
    if (count == 0 || !d.ok) return;

    begin_frame_work(c);
    if (!sync_target(c)) return;

    const glsl::Shader &vs = *p.vsc;
    const glsl::Shader &fs = *p.fsc;
    const glsl::FragmentPlan &fp = fs.frag;
    const glsl::VertexPlan &vp = vs.vert;
    VertexArray &vao = c->cur_vao();

    /* 1. 索引 */
    std::vector<uint32_t> idx;
    uint32_t lo = static_cast<uint32_t>(first);
    uint32_t hi = static_cast<uint32_t>(first) + static_cast<uint32_t>(count) - 1u; /* 无符号：不会有符号溢出 */
    if (indexed) {
        int isz;
        switch (itype) {
        case GL_UNSIGNED_BYTE: isz = 1; break;
        case GL_UNSIGNED_SHORT: isz = 2; break;
        case GL_UNSIGNED_INT: isz = 4; break;
        default: return set_error(GL_INVALID_ENUM);
        }
        const uint8_t *src;
        size_t limit = SIZE_MAX;
        if (vao.element_buffer) {
            auto bit = c->sh->buffers.find(vao.element_buffer);
            if (bit == c->sh->buffers.end()) return set_error(GL_INVALID_OPERATION);
            const size_t off = reinterpret_cast<uintptr_t>(indices);
            if (off > bit->second.data.size()) return set_error(GL_INVALID_OPERATION);
            src = bit->second.data.data() + off;
            limit = bit->second.data.size() - off;
        } else {
            if (!indices) return set_error(GL_INVALID_OPERATION);
            src = static_cast<const uint8_t *>(indices);
        }
        if (static_cast<size_t>(count) * isz > limit) return set_error(GL_INVALID_OPERATION);
        idx.resize(static_cast<size_t>(count));
        lo = UINT32_MAX;
        hi = 0;
        for (GLsizei i = 0; i < count; i++) {
            uint32_t v;
            if (isz == 1) v = src[i];
            else if (isz == 2) {
                uint16_t s;
                std::memcpy(&s, src + i * 2, 2);
                v = s;
            } else {
                std::memcpy(&v, src + i * 4, 4);
            }
            idx[static_cast<size_t>(i)] = v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    /* 顶点表：默认是连续区间 [lo, hi]；索引很稀疏时（大 VBO 里画一小块）只取用到的顶点 */
    std::vector<uint32_t> vlist;
    uint64_t nv64 = static_cast<uint64_t>(hi) - lo + 1;
    if (indexed && nv64 > 2u * static_cast<uint64_t>(count) + 64u) {
        vlist = idx;
        std::sort(vlist.begin(), vlist.end());
        vlist.erase(std::unique(vlist.begin(), vlist.end()), vlist.end());
        for (auto &v : idx) v = static_cast<uint32_t>(std::lower_bound(vlist.begin(), vlist.end(), v) - vlist.begin());
        nv64 = vlist.size();
        lo = 0;
    } else if (indexed) {
        for (auto &v : idx) v -= lo;
    }
    if (nv64 > 4u * 1024 * 1024) return set_error(GL_OUT_OF_MEMORY);
    const uint32_t nv = static_cast<uint32_t>(nv64);
    auto vid = [&](uint32_t v) { return vlist.empty() ? lo + v : vlist[v]; };

    /* 2. 属性来源 */
    std::vector<Fetch> fetch(vs.inputs.size());
    for (size_t i = 0; i < vs.inputs.size(); i++) {
        const int loc = p.attrib_loc[i];
        Fetch &f = fetch[i];
        const AttribArray &a = vao.attribs[loc];
        std::memcpy(f.konst, c->generic_attr[loc].data(), sizeof(f.konst));
        if (!a.enabled) continue;
        f.size = a.size;
        f.type = a.type;
        f.normalized = a.normalized;
        f.stride = a.stride ? a.stride : a.size * type_size(a.type);
        if (a.buffer) {
            auto bit = c->sh->buffers.find(a.buffer);
            if (bit == c->sh->buffers.end()) return set_error(GL_INVALID_OPERATION);
            const size_t off = reinterpret_cast<uintptr_t>(a.pointer);
            if (off > bit->second.data.size()) return set_error(GL_INVALID_OPERATION);
            f.base = bit->second.data.data() + off;
            f.limit = bit->second.data.size() - off;
        } else {
            if (!a.pointer) continue;
            f.base = static_cast<const uint8_t *>(a.pointer);
            f.limit = SIZE_MAX;
        }
    }

    /* 3. 片元常量表（VM 常量 / 内置模式的常量色都从这里取） */
    if (fp.program) {
        /* 隐藏 uniform：gl_FragCoord.xy = v6.xy * (kx, ky) + (bx, by)（见编译器 make_fragcoord） */
        for (const auto &u : p.link.uniforms) {
            if (u.name != "@fragcoord") continue;
            auto &v = p.storage[static_cast<size_t>(u.storage)];
            if (d.target_tex == 0) {
                const float sy = static_cast<float>(c->win_h) / static_cast<float>(d.height);
                v = {static_cast<float>(c->win_w) / static_cast<float>(d.width), -sy, 0.0f, static_cast<float>(c->win_h)};
            } else {
                v = {1.0f, 1.0f, 0.0f, 0.0f};
            }
        }
    }
    float consts[ESPGPU_VM_CONSTS][4] = {};
    const int nconsts = frag_consts(p, consts);
    /* 内置模式：颜色里与片元无关的因子 K（逐分量连乘） */
    float K[4] = {1, 1, 1, 1};
    if (!fp.program) {
        for (int slot : fp.color_slots) {
            if (slot < 0 || slot >= nconsts) continue;
            for (int k = 0; k < 4; k++) K[k] *= consts[slot][k];
        }
    }
    const bool scale_color = !fp.program && fp.color_input >= 0 && (K[0] != 1 || K[1] != 1 || K[2] != 1 || K[3] != 1);

    /* 4. 设备顶点流：POSITION + 片元要的每个 varying 槽 */
    const bool tnl = vp.tnl;
    struct Stream {
        uint8_t slot = 0;
        int comps = 4;
        uint8_t type = ESPGPU_TYPE_F32;
        uint8_t norm = 0;
        int tsize = 4;
        const Fetch *raw = nullptr; /* 非空：按源格式原样拷贝（T&L 直通） */
        std::vector<float> f;       /* 否则：nv × comps 个 float */
    };
    std::vector<Stream> streams;
    struct Out {
        int slot;
        int comps;
        int vs_out; /* VS 输出下标；-1 = 常量 0 */
    };
    std::vector<Out> outs;
    for (const auto &vsl : fp.varying_slots) {
        const int fs_in = vsl.first;
        Out o;
        o.slot = vsl.second;
        o.comps = fs.inputs[static_cast<size_t>(fs_in)].components;
        o.vs_out = p.link.vs_out_for_fs_in[static_cast<size_t>(fs_in)];
        outs.push_back(o);
    }
    if (outs.size() + 1 > ESPGPU_MAX_VERTEX_ATTRIBS) outs.resize(ESPGPU_MAX_VERTEX_ATTRIBS - 1);

    /* 源格式能否原样交给设备（设备的整数解码与 GLES 3.0 一致；半精度设备不认） */
    auto raw_ok = [&](const Fetch &f, uint8_t slot, int need, Stream &st) {
        if (!f.base) return false;
        uint8_t t;
        int ts;
        switch (f.type) {
        case GL_BYTE: t = ESPGPU_TYPE_I8; ts = 1; break;
        case GL_UNSIGNED_BYTE: t = ESPGPU_TYPE_U8; ts = 1; break;
        case GL_SHORT: t = ESPGPU_TYPE_I16; ts = 2; break;
        case GL_UNSIGNED_SHORT: t = ESPGPU_TYPE_U16; ts = 2; break;
        case GL_INT: t = ESPGPU_TYPE_I32; ts = 4; break;
        case GL_UNSIGNED_INT: t = ESPGPU_TYPE_U32; ts = 4; break;
        case GL_FLOAT: t = ESPGPU_TYPE_F32; ts = 4; break;
        case GL_FIXED: t = ESPGPU_TYPE_FIXED16_16; ts = 4; break;
        default: return false;
        }
        /* 分量不足时设备的缺省值：POSITION / TEXCOORD = (0,0,0,1) 与 GL 相同；COLOR 缺省 1、NORMAL (0,0,1,0) 只在缺 w / 不缺时一致 */
        if (f.size < need) {
            if (slot == ESPGPU_ATTR_NORMAL) return false;
            if (slot == ESPGPU_ATTR_COLOR && f.size < 3) return false;
        }
        /* 整个顶点范围都在缓冲内 */
        const uint32_t vmax = vlist.empty() ? hi : vlist.back();
        if (f.limit != SIZE_MAX &&
            static_cast<uint64_t>(vmax) * static_cast<uint64_t>(f.stride) + static_cast<uint64_t>(ts) * f.size > f.limit) {
            return false;
        }
        st.raw = &f;
        st.type = t;
        st.tsize = ts;
        st.norm = f.normalized ? 1 : 0;
        st.comps = std::min(f.size, need);
        return true;
    };
    auto float_stream = [&](uint8_t slot, int comps) {
        Stream st;
        st.slot = slot;
        st.comps = comps;
        st.f.resize(static_cast<size_t>(nv) * static_cast<size_t>(comps));
        return st;
    };

    if (tnl) {
        Stream ps;
        ps.slot = ESPGPU_ATTR_POSITION;
        const Fetch &pf = fetch[static_cast<size_t>(vp.pos_input)];
        if (!raw_ok(pf, ESPGPU_ATTR_POSITION, vp.pos_components, ps)) {
            ps = float_stream(ESPGPU_ATTR_POSITION, vp.pos_components);
            for (uint32_t v = 0; v < nv; v++) {
                float t[4];
                pf.get(vid(v), t);
                for (int k = 0; k < ps.comps; k++) ps.f[static_cast<size_t>(v) * ps.comps + k] = t[k];
            }
        }
        streams.push_back(std::move(ps));
        for (const auto &o : outs) {
            const int in = o.vs_out >= 0 ? vp.varying_input[static_cast<size_t>(o.vs_out)] : -1;
            const bool is_color = o.slot == ESPGPU_ATTR_COLOR && !fp.program;
            Stream st;
            st.slot = static_cast<uint8_t>(o.slot);
            /* 内置模式的顶点色：源已是单字节类型才原样转发，否则走 float 再压成 U8（见下） */
            if (in >= 0 && !(is_color && scale_color) && raw_ok(fetch[static_cast<size_t>(in)], st.slot, o.comps, st) &&
                (!is_color || st.tsize == 1)) {
                streams.push_back(std::move(st));
                continue;
            }
            st = float_stream(static_cast<uint8_t>(o.slot), o.comps);
            for (uint32_t v = 0; v < nv; v++) {
                float t[4] = {0, 0, 0, 0};
                if (in >= 0) fetch[static_cast<size_t>(in)].get(vid(v), t);
                for (int k = 0; k < o.comps; k++) st.f[static_cast<size_t>(v) * o.comps + k] = is_color ? t[k] * K[k] : t[k];
            }
            streams.push_back(std::move(st));
        }
        /* MVP = U0 * U1 * ...（GL 列主序）→ 设备行主序 */
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (int ui : vp.mvp_uniforms) {
            const glsl::ProgramUniform *pu = nullptr;
            for (const auto &u : p.link.uniforms) {
                if (u.vs_leaf == ui) pu = &u;
            }
            if (!pu) continue;
            float col[16];
            for (int cc = 0; cc < 4; cc++)
                for (int r = 0; r < 4; r++) col[cc * 4 + r] = p.storage[static_cast<size_t>(pu->storage + cc)][static_cast<size_t>(r)];
            mat_mul(m, col, m);
        }
        float row[16];
        for (int r = 0; r < 4; r++)
            for (int cc = 0; cc < 4; cc++) row[r * 4 + cc] = m[cc * 4 + r];
        send_mvp(row);
    } else {
        /* 主机执行顶点着色器：整批一次，只依赖 uniform 的子表达式只算一次 */
        std::vector<std::array<float, 4>> un;
        vs_uniform_slots(p, un);
        const size_t nin = vs.inputs.size();
        const size_t nout = 1 + vs.outputs.size();
        std::vector<std::array<float, 4>> in(std::max<size_t>(1, static_cast<size_t>(nv) * nin));
        std::vector<std::array<float, 4>> res(static_cast<size_t>(nv) * nout);
        for (uint32_t v = 0; v < nv; v++) {
            for (size_t i = 0; i < nin; i++) fetch[i].get(vid(v), in[static_cast<size_t>(v) * nin + i].data());
        }
        glsl::run_vertex_batch(vs, reinterpret_cast<const float(*)[4]>(un.data()), nv,
                               reinterpret_cast<const float(*)[4]>(in.data()), reinterpret_cast<float(*)[4]>(res.data()));
        /* 裁剪坐标：整批 w 都是 1（2D）就只发 xyz，z 也都是 0 就只发 xy（设备补 z=0 w=1） */
        bool w1 = true, z0 = true;
        for (uint32_t v = 0; v < nv; v++) {
            const auto &pos = res[static_cast<size_t>(v) * nout];
            w1 = w1 && pos[3] == 1.0f;
            z0 = z0 && pos[2] == 0.0f;
        }
        Stream ps = float_stream(ESPGPU_ATTR_POSITION, w1 ? (z0 ? 2 : 3) : 4);
        for (uint32_t v = 0; v < nv; v++) {
            for (int k = 0; k < ps.comps; k++) ps.f[static_cast<size_t>(v) * ps.comps + k] = res[static_cast<size_t>(v) * nout][static_cast<size_t>(k)];
        }
        streams.push_back(std::move(ps));
        for (const auto &o : outs) {
            const bool is_color = o.slot == ESPGPU_ATTR_COLOR && !fp.program;
            Stream st = float_stream(static_cast<uint8_t>(o.slot), o.comps);
            for (uint32_t v = 0; v < nv; v++) {
                for (int k = 0; k < o.comps; k++) {
                    const float x = o.vs_out >= 0 ? res[static_cast<size_t>(v) * nout + 1 + static_cast<size_t>(o.vs_out)][static_cast<size_t>(k)] : 0.0f;
                    st.f[static_cast<size_t>(v) * o.comps + k] = is_color ? x * K[k] : x;
                }
            }
            streams.push_back(std::move(st));
        }
        send_mvp(nullptr);
    }
    /* 内置模式的顶点色设备最终按 8 位用：值都在 [0,1] 就按 U8 归一化发（16 → 4 字节/顶点） */
    for (auto &st : streams) {
        if (st.raw || st.slot != ESPGPU_ATTR_COLOR || fp.program) continue;
        bool in01 = true;
        for (float x : st.f) in01 = in01 && x >= 0.0f && x <= 1.0f;
        if (!in01) continue;
        st.type = ESPGPU_TYPE_U8;
        st.norm = 1;
        st.tsize = 1;
    }

    espgpu_vertex_layout_t layout{};
    int off = 0;
    for (const auto &st : streams) {
        espgpu_vertex_attrib_t &at = layout.attribs[layout.attrib_count++];
        at.slot = st.slot;
        at.type = st.type;
        at.components = static_cast<uint8_t>(st.comps);
        at.normalized = st.norm;
        at.offset = static_cast<uint16_t>(off);
        at.frac_bits = 0;
        off += st.comps * st.tsize;
    }
    layout.stride = static_cast<uint16_t>(off);
    layout.index_type = 0;
    const size_t stride = static_cast<size_t>(off);
    std::vector<uint8_t> verts(static_cast<size_t>(nv) * stride);
    {
        size_t so = 0;
        for (const auto &st : streams) {
            const size_t bytes = static_cast<size_t>(st.comps) * st.tsize;
            if (st.raw) {
                const Fetch &f = *st.raw;
                for (uint32_t v = 0; v < nv; v++) {
                    std::memcpy(&verts[v * stride + so], f.base + static_cast<size_t>(vid(v)) * f.stride, bytes);
                }
            } else if (st.type == ESPGPU_TYPE_U8) {
                for (uint32_t v = 0; v < nv; v++) {
                    for (int k = 0; k < st.comps; k++) verts[v * stride + so + k] = to8(st.f[static_cast<size_t>(v) * st.comps + k]);
                }
            } else {
                for (uint32_t v = 0; v < nv; v++) std::memcpy(&verts[v * stride + so], &st.f[static_cast<size_t>(v) * st.comps], bytes);
            }
            so += bytes;
        }
    }

    /* 5. 纹理 */
    for (size_t u = 0; u < fp.unit_sampler.size() && u < static_cast<size_t>(kDeviceTexUnits); u++) {
        const auto [leaf, elem] = fp.unit_sampler[u];
        const glsl::ProgramUniform *pu = fs_uniform(p, leaf);
        int glunit = pu ? static_cast<int>(p.storage[static_cast<size_t>(pu->storage + elem)][0]) : 0;
        if (glunit < 0 || glunit >= kMaxTexUnits) glunit = 0;
        const GLuint tid = c->tex2d[glunit];
        auto tit = tid ? c->sh->textures.find(tid) : c->sh->textures.end();
        if (tit == c->sh->textures.end() || tit->second.width <= 0) {
            bind_unit(static_cast<int>(u), 0, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT, ESPGPU_WRAP_REPEAT);
            continue;
        }
        Texture &t = tit->second;
        if (t.device_only && tid == d.target_tex) warn_once("sampling the texture that is being rendered to");
        if ((t.dirty || !t.resident) && !t.device_only) upload_texture(c, tid, t);
        t.last_use = d.frame_counter;
        bind_unit(static_cast<int>(u), tid, filter_of(t), wrap_mode(t.wrap_s), wrap_mode(t.wrap_t));
    }

    /* 6. 管线 */
    espgpu_pipeline_t pipe{};
    pipe.mask = ESPGPU_PS_BLEND | ESPGPU_PS_DEPTH | ESPGPU_PS_ALPHA_TEST | ESPGPU_PS_CULL | ESPGPU_PS_SHADE | ESPGPU_PS_FOG;
    pipe.blend_enable = c->blend;
    pipe.blend_src = blend_factor(c->blend_src_rgb);
    pipe.blend_dst = blend_factor(c->blend_dst_rgb);
    pipe.blend_op = blend_op(c->blend_eq_rgb);
    pipe.depth_test = c->depth_test;
    pipe.depth_write = c->depth_test && c->depth_mask; /* GL：关闭深度测试时也不写深度 */
    pipe.depth_func = static_cast<uint8_t>(c->depth_func - GL_NEVER);
    pipe.cull_mode = !c->cull ? ESPGPU_CULL_NONE
                     : c->cull_face == GL_FRONT ? ESPGPU_CULL_FRONT
                     : c->cull_face == GL_BACK ? ESPGPU_CULL_BACK
                                               : ESPGPU_CULL_BOTH;
    pipe.front_face = c->front_face == GL_CCW ? ESPGPU_FRONT_CCW : ESPGPU_FRONT_CW;
    pipe.fog_enable = 0;
    espgpu_pipeline_ext_t ext{};
    ext.shade_model = ESPGPU_SHADE_MODEL_SMOOTH;
    ext.color_write = (c->color_mask[0] || c->color_mask[1] || c->color_mask[2]) ? 1 : 0;
    ext.line_width = static_cast<uint8_t>(std::min(16.0f, std::max(1.0f, std::round(c->line_width))));
    ext.point_size = 1;
    if (c->polygon_offset) {
        /* 粗略：每单位约 1/65536 深度 */
        ext.depth_bias = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, -c->poly_units * 4.0f - c->poly_factor * 4.0f)));
    }
    if (fp.program) {
        pipe.shade_mode = ESPGPU_SHADE_PROGRAM;
        ext.program_id = static_cast<uint8_t>(ensure_program(p));
        upload_consts(consts, nconsts);
        pipe.alpha_test = 0;
        pipe.alpha_func = ESPGPU_CMP_ALWAYS;
    } else {
        pipe.shade_mode = fp.shade_mode;
        pipe.alpha_test = fp.alpha_test;
        pipe.alpha_func = fp.alpha_func;
        pipe.alpha_ref = fp.alpha_ref;
        /* 没有顶点色时颜色 = 常量色 K（COLOR 命令）；TEX 模式不用颜色 */
        const bool need = fp.color_input < 0 && (fp.color_white || !fp.color_slots.empty());
        if (need) {
            uint8_t col[4];
            for (int k = 0; k < 4; k++) col[k] = to8(K[k]);
            if (!d.color_valid || std::memcmp(col, d.color, 4) != 0) {
                espgpu_cmd_color(d.dev, col[0], col[1], col[2], col[3]);
                std::memcpy(d.color, col, 4);
                d.color_valid = true;
            }
        }
    }
    if (!d.pipe_valid || std::memcmp(&pipe, &d.pipe, sizeof(pipe)) != 0 || std::memcmp(&ext, &d.pipe_ext, sizeof(ext)) != 0) {
        const espgpu_pipeline_fog_t fog{0.0f, 1.0f, 1.0f};
        espgpu_cmd_pipeline(d.dev, &pipe, &fog, &ext);
        d.pipe = pipe;
        d.pipe_ext = ext;
        d.pipe_valid = true;
    }
    if (!d.layout_valid || std::memcmp(&layout, &d.layout, sizeof(layout)) != 0) {
        espgpu_cmd_vertex_layout(d.dev, &layout);
        d.layout = layout;
        d.layout_valid = true;
    }

    /* 7. 绘制 */
    if (indexed) {
        if (nv <= 65536) {
            std::vector<uint16_t> i16(idx.size());
            for (size_t i = 0; i < idx.size(); i++) i16[i] = static_cast<uint16_t>(idx[i]);
            espgpu_cmd_draw(d.dev, prim, verts.data(), nv, static_cast<uint32_t>(stride), i16.data(),
                            static_cast<uint32_t>(i16.size()), 1);
        } else {
            espgpu_cmd_draw(d.dev, prim, verts.data(), nv, static_cast<uint32_t>(stride), idx.data(),
                            static_cast<uint32_t>(idx.size()), 2);
        }
    } else {
        espgpu_cmd_draw(d.dev, prim, verts.data(), nv, static_cast<uint32_t>(stride), nullptr, 0, 0);
    }
}

void clear(GLbitfield mask)
{
    Context *c = current();
    if (!c) return;
    Device &d = device();
    if (!d.ok) return;
    begin_frame_work(c);
    if (!sync_target(c)) return;
    uint8_t flags = 0;
    if ((mask & GL_COLOR_BUFFER_BIT) && (c->color_mask[0] || c->color_mask[1] || c->color_mask[2])) flags |= 1;
    if ((mask & GL_DEPTH_BUFFER_BIT) && c->depth_mask) flags |= 2;
    if (!flags) return;
    const uint16_t col = to565(c->clear_color[0], c->clear_color[1], c->clear_color[2]);
    const uint16_t depth = static_cast<uint16_t>(std::lround(std::min(std::max(c->clear_depth, 0.0f), 1.0f) * 65535.0f));
    espgpu_cmd_clear_ex(d.dev, flags, col, depth);
}

void swap_buffers()
{
    Context *c = current();
    if (!c) return;
    Device &d = device();
    if (!d.ok) return;
    drain_events(c);

    /* 屏幕目标必须是当前目标才能呈现 */
    if (d.target_tex != 0 || !d.target_valid) {
        espgpu_fbo_t f{};
        f.action = ESPGPU_FBO_BIND_DEFAULT;
        espgpu_cmd_fbo(d.dev, &f);
        d.target_valid = true;
        d.target_tex = 0;
        d.target_w = d.width;
        d.target_h = d.height;
        d.vp_valid = d.sc_valid = false;
    }
    /* 背压：设备比应用慢时（例如片元 VM 的重 shader，一帧要 1 秒以上），
     * 必须在提交这一帧之前把上一帧等完。否则命令会一直往设备 RX FIFO 里堆，
     * 画面永远停在几千帧之前。swap_interval > 0 时给足时间；超时说明设备
     * 确实跟不上，这一帧直接丢掉，下一轮再试。 */
    const int wait_ms = c->swap_interval > 0 ? 5000 : 30;
    if (d.inflight) {
        const int rc = espgpu_wait_frame(d.dev, d.inflight_before, wait_ms);
        d.inflight = false;
        if (rc == 0) {
            espgpu_cmd_discard(d.dev);
            if (c->hwnd) {
                RECT rc2;
                GetClientRect(c->hwnd, &rc2);
                c->win_w = std::max(1, static_cast<int>(rc2.right - rc2.left));
                c->win_h = std::max(1, static_cast<int>(rc2.bottom - rc2.top));
                HDC dc = GetDC(c->hwnd);
                if (dc) {
                    espgpu_blit_to_hdc(d.dev, dc, c->win_w, c->win_h);
                    ReleaseDC(c->hwnd, dc);
                }
            }
            return;
        }
    }

    const uint32_t before = espgpu_frame_id(d.dev);
    espgpu_cmd_present(d.dev, 500);
    d.frame_counter++;
    d.inflight_before = before;
    d.inflight = true;

    /*
     * ESPGPU_GLES_SYNC=1 时逐帧同步：这一帧发出去就等它画完，
     * 不再和下一次提交重叠。
     */
    if (d.sync_swap) {
        espgpu_wait_frame(d.dev, before, wait_ms);
        d.inflight = false;
    }

    if (c->hwnd) {
        RECT rc;
        GetClientRect(c->hwnd, &rc);
        c->win_w = std::max(1, static_cast<int>(rc.right - rc.left));
        c->win_h = std::max(1, static_cast<int>(rc.bottom - rc.top));
        HDC dc = GetDC(c->hwnd);
        if (dc) {
            espgpu_blit_to_hdc(d.dev, dc, c->win_w, c->win_h);
            ReleaseDC(c->hwnd, dc);
        }
    }
}

void finish()
{
    Device &d = device();
    if (!d.ok) return;
    const uint32_t v = ++d.fence_seq;
    espgpu_cmd_fence(d.dev, v);
    espgpu_cmd_flush(d.dev, 500);
    espgpu_wait_fence(d.dev, v, 1000);
}

void flush()
{
    Device &d = device();
    if (!d.ok) return;
    espgpu_cmd_flush(d.dev, 500);
}

void texture_deleted(GLuint id)
{
    Context *c = current();
    Device &d = device();
    if (!d.ok) return;
    if (c) {
        auto it = c->sh->textures.find(id);
        if (it != c->sh->textures.end() && it->second.resident) {
            d.resident_bytes -= std::min(d.resident_bytes, it->second.dev_bytes);
        }
    }
    espgpu_cmd_delete_texture(d.dev, id);
    for (auto &u : d.units) {
        if (u.id == id) u.valid = false;
    }
    if (d.target_tex == id) d.target_valid = false;
}

void copy_tex_image(GLuint tex, GLint x, GLint y, GLsizei w, GLsizei h)
{
    Context *c = current();
    if (!c) return;
    Device &d = device();
    if (!d.ok) return;
    if (!sync_target(c)) return;
    const Target t = desired_target(c);
    GLint r[4] = {x, y, w, h};
    espgpu_rect_t src = map_rect(c, t, r);
    espgpu_fbo_t f{};
    f.action = ESPGPU_FBO_COPY_TO_TEXTURE;
    f.texture_id = tex;
    f.width = static_cast<uint16_t>(w);
    f.height = static_cast<uint16_t>(h);
    f.x = src.x;
    f.y = src.y;
    espgpu_cmd_fbo(d.dev, &f);
    Texture &tx = c->sh->textures[tex];
    if (!tx.resident) d.resident_bytes += static_cast<uint32_t>(w * h * 2);
    tx.resident = true;
    tx.device_only = true;
    tx.dirty = false;
    tx.dev_bytes = static_cast<uint32_t>(w * h * 2);
    for (auto &u : d.units) {
        if (u.id == tex) u.valid = false;
    }
}

void read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *out)
{
    Context *c = current();
    if (!c) return;
    Device &d = device();
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
        warn_once("glReadPixels formats other than GL_RGBA/GL_UNSIGNED_BYTE");
        return set_error(GL_INVALID_OPERATION);
    }
    if (c->read_fbo) warn_once("glReadPixels from an FBO (returns the last presented frame)");
    if (w == 0 || h == 0) return;
    uint8_t *dst = static_cast<uint8_t *>(out);
    const int row_px = c->pack_row_length > 0 ? c->pack_row_length : w;
    const int align = std::max(1, c->pack_alignment);
    const size_t row_bytes = (static_cast<size_t>(row_px) * 4 + align - 1) / align * align;
    if (c->pixel_pack_buffer) {
        auto it = c->sh->buffers.find(c->pixel_pack_buffer);
        if (it == c->sh->buffers.end()) return set_error(GL_INVALID_OPERATION);
        /* PBO：写入范围必须在缓冲内 */
        const size_t off = reinterpret_cast<uintptr_t>(out);
        const size_t need = (static_cast<size_t>(c->pack_skip_rows) + static_cast<size_t>(h) - 1) * row_bytes +
                            (static_cast<size_t>(c->pack_skip_pixels) + static_cast<size_t>(w)) * 4;
        if (it->second.mapped || off > it->second.data.size() || need > it->second.data.size() - off) {
            return set_error(GL_INVALID_OPERATION);
        }
        dst = it->second.data.data() + off;
    } else if (!dst) {
        return;
    }
    /* 设备只在 PRESENT 时回传帧：这里读的是最近一次呈现的帧（SwapBuffers 之后读截图的常见用法）。
     * SwapBuffers 允许一帧在路上，先把它等回来 */
    if (d.ok && d.inflight) {
        espgpu_wait_frame(d.dev, d.inflight_before, 500);
        d.inflight = false;
    }
    std::vector<uint8_t> frame(static_cast<size_t>(d.width) * d.height * 4);
    int fw = 0, fh = 0;
    uint32_t fid = 0;
    const bool have = d.ok && espgpu_frame_snapshot_bgra(d.dev, frame.data(), frame.size(), 0, &fw, &fh, &fid) == 0;
    for (int j = 0; j < h; j++) {
        uint8_t *row = dst + static_cast<size_t>(j + c->pack_skip_rows) * row_bytes + static_cast<size_t>(c->pack_skip_pixels) * 4;
        for (int i = 0; i < w; i++) {
            uint8_t *o = row + static_cast<size_t>(i) * 4;
            if (!have) {
                o[0] = o[1] = o[2] = 0;
                o[3] = 255;
                continue;
            }
            /* 窗口坐标（左下原点）→ 设备帧（左上原点） */
            const int wx = x + i, wy = y + j;
            int fx = static_cast<int>(static_cast<int64_t>(wx) * fw / std::max(1, c->win_w));
            int fy = fh - 1 - static_cast<int>(static_cast<int64_t>(wy) * fh / std::max(1, c->win_h));
            fx = std::min(std::max(fx, 0), fw - 1);
            fy = std::min(std::max(fy, 0), fh - 1);
            const uint8_t *s = &frame[(static_cast<size_t>(fy) * fw + fx) * 4];
            o[0] = s[2];
            o[1] = s[1];
            o[2] = s[0];
            o[3] = 255;
        }
    }
}

} // namespace gles
