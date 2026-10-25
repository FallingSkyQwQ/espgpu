/*
 * gl_texture.cpp — 纹理对象、格式转换、主机侧 LRU 镜像（PLAN §5「PC 侧权重 + S3 侧 LRU」）
 *
 * 格式选择：
 *   ALPHA                  → RGBA4444（rgb = 1，与 GL 的 ALPHA 纹理在 MODULATE 下等价）
 *   LUMINANCE              → R8
 *   LUMINANCE_ALPHA / INTENSITY → RGBA4444
 *   RGB / RGBA             → 若不超过 256 种颜色 → INDEX8 + 调色板（RGB565 + 8 位 alpha）
 *                            否则不透明 → RGB565，带 alpha → RGBA4444
 *   COLOR_INDEX（paletted）→ INDEX8 + 纹理 / 共享调色板
 * Doom 的纹理本来就是 256 色，自动调色板化让它们走 1 B/px，颜色仍是 RGB565 精度，
 * alpha 也不会被 4444 量化。
 *
 * LRU：主机按设备上报的 texture_cache_size 留 15% 余量记账，超了就主动 DELETE 最久未用的，
 * 设备侧真正的 LRU 换出（ESPGPU_EVT_TEXTURE_EVICTED）只作为兜底。
 */

#include "gl_context.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace espgl {

namespace {

/* GL 1.2 打包像素类型 */
constexpr GLenum kU565 = 0x8363, kU565R = 0x8364, kU4444 = 0x8033, kU4444R = 0x8365;
constexpr GLenum kU5551 = 0x8034, kU1555R = 0x8366, kU8888 = 0x8035, kU8888R = 0x8367;
constexpr GLenum kBGR = 0x80E0, kBGRA = 0x80E1;

enum Base { B_ALPHA, B_LUM, B_LUM_ALPHA, B_INTENSITY, B_RGB, B_RGBA, B_INDEX };

Base base_format(GLint ifmt, GLenum format)
{
    if (format == GL_COLOR_INDEX) {
        return B_INDEX;
    }
    switch (ifmt) {
    case GL_ALPHA:
    case 0x803B: case 0x803C: case 0x803D: case 0x803E: return B_ALPHA;
    case 1:
    case GL_LUMINANCE:
    case 0x803F: case 0x8040: case 0x8041: case 0x8042: return B_LUM;
    case 2:
    case GL_LUMINANCE_ALPHA:
    case 0x8043: case 0x8044: case 0x8045: case 0x8046: case 0x8047: case 0x8048: return B_LUM_ALPHA;
    case GL_INTENSITY:
    case 0x804A: case 0x804B: case 0x804C: case 0x804D: return B_INTENSITY;
    case 3:
    case GL_RGB:
    case kBGR:
    case 0x2A10 /* GL_R3_G3_B2 */:
    case 0x804F: case 0x8050: case 0x8051: case 0x8052: case 0x8053: case 0x8054: return B_RGB;
    case 0x80E2: case 0x80E3: case 0x80E4: case 0x80E5: case 0x80E6: case 0x80E7: return B_INDEX;
    default: return B_RGBA;
    }
}

int format_comps(GLenum format)
{
    switch (format) {
    case GL_RGBA:
    case kBGRA: return 4;
    case GL_RGB:
    case kBGR: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    default: return 1;
    }
}

int type_bytes(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:
    case GL_BYTE: return 1;
    case GL_UNSIGNED_SHORT:
    case GL_SHORT: return 2;
    case GL_UNSIGNED_INT:
    case GL_INT:
    case GL_FLOAT: return 4;
    default: return 0;
    }
}

bool is_packed(GLenum type, int *bytes)
{
    switch (type) {
    case kU565: case kU565R: case kU4444: case kU4444R: case kU5551: case kU1555R: *bytes = 2; return true;
    case kU8888: case kU8888R: *bytes = 4; return true;
    case 0x8032: case 0x8362: *bytes = 1; return true; /* 3_3_2 */
    default: return false;
    }
}

/** 解包像素：每个分量规格化到 0..255 */
void unpack_pixel(const uint8_t *p, GLenum type, int comps, bool swap, uint8_t out[4], uint32_t *index)
{
    int pb;
    if (is_packed(type, &pb)) {
        uint32_t v = 0;
        if (pb == 1) {
            v = p[0];
        } else if (pb == 2) {
            uint16_t t;
            memcpy(&t, p, 2);
            if (swap) t = static_cast<uint16_t>((t >> 8) | (t << 8));
            v = t;
        } else {
            memcpy(&v, p, 4);
            if (swap) v = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
        }
        auto ex = [](uint32_t x, int bits) { return static_cast<uint8_t>((x * 255u + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); };
        switch (type) {
        case kU565: out[0] = ex((v >> 11) & 31, 5); out[1] = ex((v >> 5) & 63, 6); out[2] = ex(v & 31, 5); out[3] = 255; break;
        case kU565R: out[2] = ex((v >> 11) & 31, 5); out[1] = ex((v >> 5) & 63, 6); out[0] = ex(v & 31, 5); out[3] = 255; break;
        case kU4444: out[0] = ex((v >> 12) & 15, 4); out[1] = ex((v >> 8) & 15, 4); out[2] = ex((v >> 4) & 15, 4); out[3] = ex(v & 15, 4); break;
        case kU4444R: out[3] = ex((v >> 12) & 15, 4); out[2] = ex((v >> 8) & 15, 4); out[1] = ex((v >> 4) & 15, 4); out[0] = ex(v & 15, 4); break;
        case kU5551: out[0] = ex((v >> 11) & 31, 5); out[1] = ex((v >> 6) & 31, 5); out[2] = ex((v >> 1) & 31, 5); out[3] = (v & 1) ? 255 : 0; break;
        case kU1555R: out[0] = ex(v & 31, 5); out[1] = ex((v >> 5) & 31, 5); out[2] = ex((v >> 10) & 31, 5); out[3] = (v >> 15) ? 255 : 0; break;
        case kU8888: out[0] = static_cast<uint8_t>(v >> 24); out[1] = static_cast<uint8_t>(v >> 16); out[2] = static_cast<uint8_t>(v >> 8); out[3] = static_cast<uint8_t>(v); break;
        case kU8888R: out[0] = static_cast<uint8_t>(v); out[1] = static_cast<uint8_t>(v >> 8); out[2] = static_cast<uint8_t>(v >> 16); out[3] = static_cast<uint8_t>(v >> 24); break;
        case 0x8032: out[0] = ex((v >> 5) & 7, 3); out[1] = ex((v >> 2) & 7, 3); out[2] = ex(v & 3, 2); out[3] = 255; break;
        default: out[0] = ex(v & 7, 3); out[1] = ex((v >> 3) & 7, 3); out[2] = ex((v >> 6) & 3, 2); out[3] = 255; break;
        }
        return;
    }
    const int tb = type_bytes(type);
    float f[4] = {0, 0, 0, 1};
    uint32_t raw0 = 0;
    for (int k = 0; k < comps && k < 4; k++) {
        const uint8_t *q = p + k * tb;
        uint8_t sw[4];
        if (swap && tb == 4) {
            sw[0] = q[3];
            sw[1] = q[2];
            sw[2] = q[1];
            sw[3] = q[0];
            q = sw;
        } else if (swap && tb == 2) {
            sw[0] = q[1];
            sw[1] = q[0];
            q = sw;
        }
        switch (type) {
        case GL_UNSIGNED_BYTE: f[k] = q[0] / 255.0f; raw0 = k == 0 ? q[0] : raw0; break;
        case GL_BYTE: { int8_t t; memcpy(&t, q, 1); f[k] = std::max(0, static_cast<int>(t)) / 127.0f; raw0 = k == 0 ? static_cast<uint32_t>(std::max(0, static_cast<int>(t))) : raw0; break; }
        case GL_UNSIGNED_SHORT: { uint16_t t; memcpy(&t, q, 2); f[k] = t / 65535.0f; raw0 = k == 0 ? t : raw0; break; }
        case GL_SHORT: { int16_t t; memcpy(&t, q, 2); f[k] = std::max(0, static_cast<int>(t)) / 32767.0f; raw0 = k == 0 ? static_cast<uint32_t>(std::max(0, static_cast<int>(t))) : raw0; break; }
        case GL_UNSIGNED_INT: { uint32_t t; memcpy(&t, q, 4); f[k] = static_cast<float>(t / 4294967295.0); raw0 = k == 0 ? t : raw0; break; }
        case GL_INT: { int32_t t; memcpy(&t, q, 4); f[k] = static_cast<float>(std::max(0, t) / 2147483647.0); raw0 = k == 0 ? static_cast<uint32_t>(std::max(0, t)) : raw0; break; }
        case GL_FLOAT: memcpy(&f[k], q, 4); raw0 = k == 0 ? static_cast<uint32_t>(std::max(0.0f, f[k])) : raw0; break;
        default: break;
        }
    }
    if (index) {
        *index = raw0;
    }
    for (int k = 0; k < 4; k++) {
        const float v = f[k] < 0 ? 0 : (f[k] > 1 ? 1 : f[k]);
        out[k] = static_cast<uint8_t>(std::lround(v * 255.0f));
    }
}

void place(GLenum format, const uint8_t c[4], uint8_t *rgba)
{
    switch (format) {
    case GL_RGBA: rgba[0] = c[0]; rgba[1] = c[1]; rgba[2] = c[2]; rgba[3] = c[3]; break;
    case GL_RGB: rgba[0] = c[0]; rgba[1] = c[1]; rgba[2] = c[2]; rgba[3] = 255; break;
    case kBGRA: rgba[0] = c[2]; rgba[1] = c[1]; rgba[2] = c[0]; rgba[3] = c[3]; break;
    case kBGR: rgba[0] = c[2]; rgba[1] = c[1]; rgba[2] = c[0]; rgba[3] = 255; break;
    case GL_LUMINANCE: rgba[0] = rgba[1] = rgba[2] = c[0]; rgba[3] = 255; break;
    case GL_LUMINANCE_ALPHA: rgba[0] = rgba[1] = rgba[2] = c[0]; rgba[3] = c[1]; break;
    case GL_ALPHA: rgba[0] = rgba[1] = rgba[2] = 0; rgba[3] = c[0]; break;
    case GL_RED: rgba[0] = c[0]; rgba[1] = 0; rgba[2] = 0; rgba[3] = 255; break;
    case GL_GREEN: rgba[0] = 0; rgba[1] = c[0]; rgba[2] = 0; rgba[3] = 255; break;
    case GL_BLUE: rgba[0] = 0; rgba[1] = 0; rgba[2] = c[0]; rgba[3] = 255; break;
    default: rgba[0] = rgba[1] = rgba[2] = c[0]; rgba[3] = 255; break;
    }
}

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

inline uint16_t rgba4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return static_cast<uint16_t>(((r * 15 + 127) / 255) << 12 | ((g * 15 + 127) / 255) << 8 |
                                 ((b * 15 + 127) / 255) << 4 | ((a * 15 + 127) / 255));
}

/** 盒式缩放 RGBA8 */
void resample_rgba(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        const int y0 = y * sh / dh;
        const int y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; x++) {
            const int x0 = x * sw / dw;
            const int x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (int yy = y0; yy < y1; yy++) {
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t *p = src + (static_cast<size_t>(yy) * sw + xx) * 4;
                    for (int k = 0; k < 4; k++) acc[k] += p[k];
                    n++;
                }
            }
            uint8_t *o = dst + (static_cast<size_t>(y) * dw + x) * 4;
            for (int k = 0; k < 4; k++) o[k] = static_cast<uint8_t>((acc[k] + n / 2) / n);
        }
    }
}

void resample_index(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        const int sy = (y * sh + sh / 2) / dh;
        for (int x = 0; x < dw; x++) {
            const int sx = (x * sw + sw / 2) / dw;
            dst[static_cast<size_t>(y) * dw + x] = src[static_cast<size_t>(std::min(sy, sh - 1)) * sw + std::min(sx, sw - 1)];
        }
    }
}

std::unordered_map<GLenum, std::pair<int, int>> g_proxy; /* 代理纹理的尺寸 */

} // namespace

/* ── 像素存储 ─────────────────────────────────────────────────────── */

void tex_unpack_to_rgba(Context *c, int w, int h, GLenum format, GLenum type, const void *pixels,
                        std::vector<uint8_t> &rgba, std::vector<uint8_t> *index_out)
{
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    if (index_out) {
        index_out->assign(static_cast<size_t>(w) * h, 0);
    }
    if (pixels == nullptr || w <= 0 || h <= 0) {
        return;
    }
    const PixelStore &ps = c ? c->cs.unpack : PixelStore();
    int pb = 0;
    const bool packed = is_packed(type, &pb);
    const int comps = format_comps(format);
    const int bpp = packed ? pb : comps * type_bytes(type);
    if (bpp == 0) {
        return;
    }
    const int row_px = ps.row_length > 0 ? ps.row_length : w;
    size_t row_bytes = static_cast<size_t>(row_px) * bpp;
    const size_t al = static_cast<size_t>(std::max(1, ps.alignment));
    /* GL：分量（打包类型则整个像素）不小于对齐值时行之间不补齐 */
    const size_t elem = static_cast<size_t>(packed ? pb : type_bytes(type));
    if (elem < al) {
        row_bytes = (row_bytes + al - 1) / al * al;
    }
    const uint8_t *base = static_cast<const uint8_t *>(pixels) + static_cast<size_t>(ps.skip_rows) * row_bytes +
                          static_cast<size_t>(ps.skip_pixels) * bpp;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = base + static_cast<size_t>(y) * row_bytes;
        for (int x = 0; x < w; x++) {
            uint8_t c4[4];
            uint32_t idx = 0;
            unpack_pixel(row + static_cast<size_t>(x) * bpp, type, comps, ps.swap_bytes, c4, &idx);
            uint8_t *o = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            place(format, c4, o);
            if (index_out) {
                (*index_out)[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(type == GL_UNSIGNED_BYTE ? row[static_cast<size_t>(x) * bpp] : (idx & 0xFF));
            }
        }
    }
}

void tex_pack_from_rgba(Context *c, int w, int h, GLenum format, GLenum type, const uint8_t *rgba, void *pixels)
{
    if (pixels == nullptr || w <= 0 || h <= 0) {
        return;
    }
    const PixelStore &ps = c ? c->cs.pack : PixelStore();
    const int comps = format_comps(format);
    const int tb = type_bytes(type);
    if (tb == 0) {
        return; /* 打包类型的回读不支持 */
    }
    const int bpp = comps * tb;
    const int row_px = ps.row_length > 0 ? ps.row_length : w;
    size_t row_bytes = static_cast<size_t>(row_px) * bpp;
    const size_t al = static_cast<size_t>(std::max(1, ps.alignment));
    if (static_cast<size_t>(tb) < al) {
        row_bytes = (row_bytes + al - 1) / al * al;
    }
    uint8_t *base = static_cast<uint8_t *>(pixels) + static_cast<size_t>(ps.skip_rows) * row_bytes +
                    static_cast<size_t>(ps.skip_pixels) * bpp;
    for (int y = 0; y < h; y++) {
        uint8_t *row = base + static_cast<size_t>(y) * row_bytes;
        for (int x = 0; x < w; x++) {
            const uint8_t *s = rgba + (static_cast<size_t>(y) * w + x) * 4;
            uint8_t v[4];
            int n = comps;
            switch (format) {
            case GL_RGBA: v[0] = s[0]; v[1] = s[1]; v[2] = s[2]; v[3] = s[3]; break;
            case GL_RGB: v[0] = s[0]; v[1] = s[1]; v[2] = s[2]; break;
            case kBGRA: v[0] = s[2]; v[1] = s[1]; v[2] = s[0]; v[3] = s[3]; break;
            case kBGR: v[0] = s[2]; v[1] = s[1]; v[2] = s[0]; break;
            case GL_LUMINANCE: v[0] = static_cast<uint8_t>((s[0] * 77 + s[1] * 151 + s[2] * 28) >> 8); break;
            case GL_LUMINANCE_ALPHA: v[0] = static_cast<uint8_t>((s[0] * 77 + s[1] * 151 + s[2] * 28) >> 8); v[1] = s[3]; break;
            case GL_ALPHA: v[0] = s[3]; break;
            case GL_RED: v[0] = s[0]; break;
            case GL_GREEN: v[0] = s[1]; break;
            case GL_BLUE: v[0] = s[2]; break;
            default: v[0] = s[0]; n = 1; break;
            }
            uint8_t *o = row + static_cast<size_t>(x) * bpp;
            for (int k = 0; k < n; k++) {
                switch (type) {
                case GL_UNSIGNED_BYTE:
                case GL_BYTE: o[k] = type == GL_BYTE ? static_cast<uint8_t>(v[k] >> 1) : v[k]; break;
                case GL_UNSIGNED_SHORT: { uint16_t t = static_cast<uint16_t>(v[k] * 257); memcpy(o + k * 2, &t, 2); break; }
                case GL_SHORT: { int16_t t = static_cast<int16_t>(v[k] * 32767 / 255); memcpy(o + k * 2, &t, 2); break; }
                case GL_UNSIGNED_INT: { uint32_t t = v[k] * 16843009u; memcpy(o + k * 4, &t, 4); break; }
                case GL_INT: { int32_t t = static_cast<int32_t>(v[k] * (2147483647.0 / 255.0)); memcpy(o + k * 4, &t, 4); break; }
                case GL_FLOAT: { float t = v[k] / 255.0f; memcpy(o + k * 4, &t, 4); break; }
                default: break;
                }
            }
        }
    }
}

/* ── 对象管理 ─────────────────────────────────────────────────────── */

static uint16_t alloc_dev_id()
{
    if (!g_sh.free_dev_ids.empty()) {
        const uint16_t id = g_sh.free_dev_ids.back();
        g_sh.free_dev_ids.pop_back();
        return id;
    }
    if (g_sh.next_dev_id == 0xFFFF) {
        return 0;
    }
    return g_sh.next_dev_id++;
}

Texture *tex_lookup(GLuint name, bool create)
{
    if (name == 0) {
        return &g_sh.default_tex;
    }
    auto it = g_sh.textures.find(name);
    if (it != g_sh.textures.end()) {
        return it->second.get();
    }
    if (!create) {
        return nullptr;
    }
    auto t = std::make_unique<Texture>();
    t->name = name;
    Texture *p = t.get();
    g_sh.textures[name] = std::move(t);
    return p;
}

static void ensure_dev_id(Texture *t)
{
    if (t->dev_id == 0) {
        t->dev_id = alloc_dev_id();
        if (t->dev_id) {
            g_sh.dev_to_name[t->dev_id] = t->name;
        }
    }
}

Texture *tex_bound(Context *c, int unit)
{
    const TexUnitState &u = c->st.unit[unit];
    const GLuint name = u.enable_2d ? u.bound_2d : u.bound_1d;
    Texture *t = tex_lookup(name, false);
    return t ? t : &g_sh.default_tex;
}

bool tex_usable(const Texture *t)
{
    return t && t->has_image && t->w > 0 && t->h > 0;
}

uint8_t tex_dev_filter(const Texture *t)
{
    return t->mag_filter == GL_LINEAR ? ESPGPU_FILTER_LINEAR : ESPGPU_FILTER_NEAREST;
}

static uint8_t dev_wrap_of(GLenum w)
{
    switch (w) {
    case GL_REPEAT: return ESPGPU_WRAP_REPEAT;
    case 0x8370 /* GL_MIRRORED_REPEAT */: return ESPGPU_WRAP_MIRROR;
    default: return ESPGPU_WRAP_CLAMP;
    }
}

/** 低 4 位 = s 方向，高 4 位 = t 方向（Doom 的天空：s 重复、t 夹取） */
uint8_t tex_dev_wrap(const Texture *t)
{
    return static_cast<uint8_t>(dev_wrap_of(t->wrap_s) | (dev_wrap_of(t->wrap_t) << 4));
}

/** 由 GL 数据生成设备形态 */
static void build_device_pixels(Texture *t)
{
    t->dirty = false;
    t->upload = true;
    const int max = static_cast<int>(std::max<uint32_t>(16, g_sh.max_tex_dim));
    int dw = std::min(t->w, max), dh = std::min(t->h, max);
    dw = std::max(dw, 1);
    dh = std::max(dh, 1);
    t->dw = dw;
    t->dh = dh;
    const size_t n = static_cast<size_t>(dw) * dh;

    const Base base = base_format(t->internal_format, t->paletted ? GL_COLOR_INDEX : GL_RGBA);

    if (t->paletted) {
        std::vector<uint8_t> idx(n);
        if (dw == t->w && dh == t->h) {
            idx = t->index;
        } else {
            resample_index(t->index.data(), t->w, t->h, idx.data(), dw, dh);
        }
        t->dev_fmt = ESPGPU_FMT_INDEX8;
        t->dev_pixels = std::move(idx);
        t->dev_bytes = static_cast<uint32_t>(n + 768);
        return;
    }

    std::vector<uint8_t> src;
    const uint8_t *rgba = t->rgba.data();
    if (dw != t->w || dh != t->h) {
        src.resize(n * 4);
        resample_rgba(t->rgba.data(), t->w, t->h, src.data(), dw, dh);
        rgba = src.data();
    }

    if (base == B_LUM) {
        t->dev_fmt = ESPGPU_FMT_R8;
        t->dev_pixels.resize(n);
        for (size_t i = 0; i < n; i++) {
            t->dev_pixels[i] = rgba[i * 4];
        }
        t->dev_bytes = static_cast<uint32_t>(n * 2);
        return;
    }

    /* 统一成「最终 RGBA」再选格式 */
    std::vector<uint8_t> fin(n * 4);
    bool opaque = true;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *s = rgba + i * 4;
        uint8_t *o = &fin[i * 4];
        switch (base) {
        case B_ALPHA: o[0] = o[1] = o[2] = 255; o[3] = s[3]; break;
        case B_LUM_ALPHA: o[0] = o[1] = o[2] = s[0]; o[3] = s[3]; break;
        case B_INTENSITY: o[0] = o[1] = o[2] = o[3] = s[0]; break;
        case B_RGB: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
        default: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
        }
        if (o[3] != 255) {
            opaque = false;
        }
    }

    /* 自动调色板化：≤256 种（RGB565 + A8）颜色 */
    if (env_int("ESPGPU_GL_AUTOPALETTE", 1) != 0 && (!opaque || n >= 1024)) {
        std::unordered_map<uint32_t, uint8_t> map;
        map.reserve(512);
        std::vector<uint8_t> idx(n);
        bool ok = true;
        for (size_t i = 0; i < n && ok; i++) {
            const uint8_t *o = &fin[i * 4];
            const uint32_t key = (static_cast<uint32_t>(rgb565(o[0], o[1], o[2])) << 8) | o[3];
            auto it = map.find(key);
            if (it == map.end()) {
                if (map.size() >= 256) {
                    ok = false;
                    break;
                }
                const uint8_t id = static_cast<uint8_t>(map.size());
                map.emplace(key, id);
                idx[i] = id;
            } else {
                idx[i] = it->second;
            }
        }
        if (ok) {
            t->dev_fmt = ESPGPU_FMT_INDEX8;
            t->dev_pixels = std::move(idx);
            t->has_palette = true;
            t->palette_size = 256;
            memset(t->palette, 0, sizeof(t->palette));
            for (auto &e : map) {
                const uint16_t c565 = static_cast<uint16_t>(e.first >> 8);
                uint8_t *p = &t->palette[e.second * 4];
                p[0] = static_cast<uint8_t>(((c565 >> 11) & 31) * 255 / 31);
                p[1] = static_cast<uint8_t>(((c565 >> 5) & 63) * 255 / 63);
                p[2] = static_cast<uint8_t>((c565 & 31) * 255 / 31);
                p[3] = static_cast<uint8_t>(e.first & 0xFF);
            }
            t->dev_bytes = static_cast<uint32_t>(n + 768);
            t->palette_format = 0; /* 标记：调色板是自动生成的 */
            return;
        }
    }

    t->dev_pixels.resize(n * 2);
    if (opaque) {
        t->dev_fmt = ESPGPU_FMT_RGB565;
        for (size_t i = 0; i < n; i++) {
            const uint16_t v = rgb565(fin[i * 4], fin[i * 4 + 1], fin[i * 4 + 2]);
            memcpy(&t->dev_pixels[i * 2], &v, 2);
        }
        t->dev_bytes = static_cast<uint32_t>(n * 2);
    } else {
        t->dev_fmt = ESPGPU_FMT_RGBA4444;
        for (size_t i = 0; i < n; i++) {
            const uint16_t v = rgba4444(fin[i * 4], fin[i * 4 + 1], fin[i * 4 + 2], fin[i * 4 + 3]);
            memcpy(&t->dev_pixels[i * 2], &v, 2);
        }
        t->dev_bytes = static_cast<uint32_t>(n * 3);
    }
}

static void device_delete(Texture *t)
{
    if (t->resident && g_sh.dev) {
        espgpu_cmd_delete_texture(g_sh.dev, t->dev_id);
    }
    if (t->resident) {
        g_sh.tex_used -= std::min<uint64_t>(g_sh.tex_used, t->dev_bytes);
    }
    t->resident = false;
    t->upload = true;
    g_sh.bind_epoch++;
}

/** 腾出 need 字节；返回 false 表示连换出都不够 */
static bool make_room(Texture *self, uint64_t need)
{
    if (need > g_sh.tex_budget) {
        return false;
    }
    while (g_sh.tex_used + need > g_sh.tex_budget) {
        Texture *victim = nullptr;
        for (int pass = 0; pass < 2 && victim == nullptr; pass++) {
            for (auto &kv : g_sh.textures) {
                Texture *t = kv.second.get();
                if (t == self || !t->resident || t->pin || t->device_only) {
                    continue;
                }
                if (pass == 0 && (t->last_used >= g_sh.frame || t->prioritized)) {
                    continue;
                }
                if (victim == nullptr || t->last_used < victim->last_used) {
                    victim = t;
                }
            }
        }
        if (victim == nullptr) {
            return false;
        }
        device_delete(victim);
    }
    return true;
}

bool tex_make_resident(Texture *t)
{
    if (!tex_usable(t) || g_sh.dev == nullptr) {
        return false;
    }
    if (t->device_only) {
        return t->resident;
    }
    if (t->dirty) {
        /* 重新指定了一模一样的图像（有的引擎每帧重传动画纹理的同一帧）：设备上的那份还能用 */
        const bool was_resident = t->resident;
        const std::vector<uint8_t> old_px = was_resident ? t->dev_pixels : std::vector<uint8_t>();
        const uint8_t old_fmt = t->dev_fmt;
        const int old_w = t->dw, old_h = t->dh;
        uint8_t old_pal[256 * 4];
        memcpy(old_pal, t->palette, sizeof(old_pal));
        build_device_pixels(t);
        if (was_resident && !t->pal_changed && t->dev_fmt == old_fmt && t->dw == old_w && t->dh == old_h &&
            t->dev_pixels == old_px &&
            (t->dev_fmt != ESPGPU_FMT_INDEX8 || memcmp(old_pal, t->palette, sizeof(old_pal)) == 0)) {
            t->upload = false;
        }
    }
    if (t->resident && !t->upload) {
        return true;
    }
    ensure_dev_id(t);
    if (t->dev_id == 0) {
        return false;
    }
    if (t->resident) {
        g_sh.tex_used -= std::min<uint64_t>(g_sh.tex_used, t->dev_bytes);
        t->resident = false;
    }
    if (!make_room(t, t->dev_bytes)) {
        log_line("texture %u (%dx%d, %u B) does not fit in the device cache", t->name, t->dw, t->dh, t->dev_bytes);
        return false;
    }
    const uint8_t flags = t->prioritized ? ESPGPU_TEXF_PRIORITY : 0;
    espgpu_cmd_upload_texture_ex(g_sh.dev, t->dev_id, static_cast<uint16_t>(t->dw), static_cast<uint16_t>(t->dh),
                                 t->dev_fmt, t->dev_pixels.data(), flags);
    if (t->dev_fmt == ESPGPU_FMT_INDEX8) {
        uint16_t pal[256];
        uint8_t alpha[256];
        const uint8_t *src = t->palette;
        int count = t->palette_size;
        if (!t->has_palette && g_sh.shared_palette_size > 0) {
            src = g_sh.shared_palette;
            count = g_sh.shared_palette_size;
        }
        for (int i = 0; i < 256; i++) {
            if (t->has_palette || g_sh.shared_palette_size > 0) {
                const uint8_t *p = src + (i < count ? i : 0) * 4;
                pal[i] = rgb565(p[0], p[1], p[2]);
                alpha[i] = p[3];
            } else {
                pal[i] = rgb565(static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i));
                alpha[i] = 255;
            }
        }
        espgpu_cmd_tex_palette(g_sh.dev, t->dev_id, 0, 256, pal, alpha);
    }
    t->resident = true;
    t->upload = false;
    t->pal_changed = false;
    g_sh.tex_used += t->dev_bytes;
    g_sh.bind_epoch++;
    return true;
}

void tex_on_evicted(uint16_t dev_id)
{
    auto it = g_sh.dev_to_name.find(dev_id);
    if (it == g_sh.dev_to_name.end()) {
        return;
    }
    Texture *t = tex_lookup(it->second, false);
    if (t && t->resident) {
        g_sh.tex_used -= std::min<uint64_t>(g_sh.tex_used, t->dev_bytes);
        t->resident = false;
        t->upload = true;
        g_sh.bind_epoch++;
        if (t->device_only) {
            t->has_image = false; /* 只存在设备上的内容丢了 */
        }
    }
}

static void tex_free(Texture *t)
{
    device_delete(t);
    if (t->dev_id) {
        g_sh.dev_to_name.erase(t->dev_id);
        g_sh.free_dev_ids.push_back(t->dev_id);
        t->dev_id = 0;
    }
}

/** 纹理的数据 / 参数要变：引用它的待发批次（flush 时才采样）先发掉 */
static void flush_if_used(Context *c, const Texture *t)
{
    bool used = false;
    auto check = [&](const Pending &p) {
        if (p.empty()) {
            return;
        }
        /* 按快照里启用单元绑定的名字比：批次开始时还不可用（没图像）的纹理变得可用也算 */
        for (int u = 0; u < kMaxUnits; u++) {
            const TexUnitState &us = p.st.unit[u];
            const GLuint name = us.enable_2d ? us.bound_2d : (us.enable_1d ? us.bound_1d : 0xFFFFFFFFu);
            used = used || (name != 0xFFFFFFFFu && tex_lookup(name, false) == t) ||
                   (name == 0 && t == &g_sh.default_tex);
        }
    };
    check(c->pending);
    for (const Pending &p : c->parked) {
        check(p);
    }
    if (used) {
        flush_pending(c);
    }
    c->key_dirty = true;
}

/* ── GL 入口 ──────────────────────────────────────────────────────── */

void impl_glGenTextures(GLsizei n, GLuint *textures)
{
    if (textures == nullptr || n < 0) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    for (GLsizei i = 0; i < n; i++) {
        while (g_sh.textures.count(g_sh.next_tex_name) || g_sh.next_tex_name == 0) {
            g_sh.next_tex_name++;
        }
        textures[i] = g_sh.next_tex_name;
        tex_lookup(g_sh.next_tex_name, true);
        g_sh.next_tex_name++;
    }
}

void impl_glDeleteTextures(GLsizei n, const GLuint *textures)
{
    ESPGL_CTX(c);
    if (textures == nullptr || n < 0) {
        return;
    }
    state_dirty(c);
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    for (GLsizei i = 0; i < n; i++) {
        const GLuint name = textures[i];
        if (name == 0) {
            continue;
        }
        auto it = g_sh.textures.find(name);
        if (it == g_sh.textures.end()) {
            continue;
        }
        tex_free(it->second.get());
        g_sh.textures.erase(it);
        for (Context *cc : g_sh.contexts) {
            for (int u = 0; u < kMaxUnits; u++) {
                if (cc->st.unit[u].bound_2d == name) cc->st.unit[u].bound_2d = 0;
                if (cc->st.unit[u].bound_1d == name) cc->st.unit[u].bound_1d = 0;
            }
        }
    }
}

GLboolean impl_glIsTexture(GLuint texture)
{
    return texture != 0 && g_sh.textures.count(texture) ? 1 : 0;
}

GLboolean impl_glAreTexturesResident(GLsizei n, const GLuint *textures, GLboolean *residences)
{
    bool all = true;
    for (GLsizei i = 0; i < n && textures; i++) {
        Texture *t = tex_lookup(textures[i], false);
        const bool r = t && t->resident;
        if (residences) residences[i] = r ? 1 : 0;
        all = all && r;
    }
    return all ? 1 : 0;
}

void impl_glPrioritizeTextures(GLsizei n, const GLuint *textures, const GLclampf *priorities)
{
    for (GLsizei i = 0; i < n && textures && priorities; i++) {
        if (Texture *t = tex_lookup(textures[i], false)) {
            t->priority = priorities[i];
            t->prioritized = priorities[i] >= 1.0f;
        }
    }
}

void impl_glBindTexture(GLenum target, GLuint texture)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glBindTexture(target, texture));
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    Texture *t = tex_lookup(texture, true);
    if (texture != 0 && t->target != target && t->has_image) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    t->target = target;
    TexUnitState &u = c->st.unit[c->st.active_unit];
    GLuint &slot = target == GL_TEXTURE_1D ? u.bound_1d : u.bound_2d;
    if (slot != texture) {
        state_changed(c);
        slot = texture;
    }
}

static Texture *target_tex(Context *c, GLenum target)
{
    const TexUnitState &u = c->st.unit[c->st.active_unit];
    const GLuint name = target == GL_TEXTURE_1D ? u.bound_1d : u.bound_2d;
    return tex_lookup(name, true);
}

static void tex_image(Context *c, GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLenum format,
                      GLenum type, const GLvoid *pixels)
{
    if (target == 0x8064 /* GL_PROXY_TEXTURE_2D */ || target == 0x8063 /* GL_PROXY_TEXTURE_1D */) {
        const bool ok = w <= static_cast<GLsizei>(g_sh.max_tex_dim) && h <= static_cast<GLsizei>(g_sh.max_tex_dim);
        g_proxy[target] = ok ? std::make_pair(static_cast<int>(w), static_cast<int>(h)) : std::make_pair(0, 0);
        return;
    }
    if (w < 0 || h < 0 || level < 0 || level >= 12) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    Texture *t = target_tex(c, target);
    t->level_w[level] = w;
    t->level_h[level] = h;
    if (level != 0) {
        return; /* 设备不做 mipmap，只用层级 0 */
    }
    flush_if_used(c, t);
    t->w = w;
    t->h = h;
    t->internal_format = ifmt;
    const bool idx = format == GL_COLOR_INDEX || base_format(ifmt, format) == B_INDEX;
    t->paletted = idx;
    if (t->palette_format == 0) {
        t->has_palette = false; /* 自动调色板随数据重建 */
        t->palette_format = GL_RGBA;
    }
    tex_unpack_to_rgba(c, w, h, format, type, pixels, t->rgba, idx ? &t->index : nullptr);
    if (idx) {
        t->rgba.clear();
    }
    t->has_image = w > 0 && h > 0;
    t->device_only = false;
    t->dirty = true;
    t->upload = true;
}

void impl_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const GLvoid *pixels)
{
    ESPGL_CTX(c);
    (void)border;
    tex_image(c, target, level, internalformat, width, height, format, type, pixels);
}

void impl_glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border, GLenum format,
                       GLenum type, const GLvoid *pixels)
{
    ESPGL_CTX(c);
    (void)border;
    tex_image(c, target, level, internalformat, width, 1, format, type, pixels);
}

static void tex_sub_image(Context *c, GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h,
                          GLenum format, GLenum type, const GLvoid *pixels)
{
    if (level != 0 || w <= 0 || h <= 0) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    Texture *t = target_tex(c, target);
    if (!t->has_image || xo < 0 || yo < 0 || xo + w > t->w || yo + h > t->h) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    if (t->device_only) {
        log_line("glTexSubImage on a copied (device-only) texture is not supported");
        return;
    }
    flush_if_used(c, t);
    std::vector<uint8_t> rgba, idx;
    tex_unpack_to_rgba(c, w, h, format, type, pixels, rgba, t->paletted ? &idx : nullptr);
    for (int y = 0; y < h; y++) {
        if (t->paletted) {
            memcpy(&t->index[(static_cast<size_t>(yo + y) * t->w + xo)], &idx[static_cast<size_t>(y) * w],
                   static_cast<size_t>(w));
        } else {
            memcpy(&t->rgba[(static_cast<size_t>(yo + y) * t->w + xo) * 4], &rgba[static_cast<size_t>(y) * w * 4],
                   static_cast<size_t>(w) * 4);
        }
    }
    if (t->palette_format == 0) {
        t->has_palette = false;
    }
    t->dirty = true;
    t->upload = true;
}

void impl_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const GLvoid *pixels)
{
    ESPGL_CTX(c);
    tex_sub_image(c, target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void impl_glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                          const GLvoid *pixels)
{
    ESPGL_CTX(c);
    tex_sub_image(c, target, level, xoffset, 0, width, 1, format, type, pixels);
}

/** 从当前帧缓冲拷贝。窗口矩形 (x,y,w,h)（左下角原点） → 设备 FBO COPY */
static void copy_tex(Context *c, GLenum target, GLint x, GLint y, GLsizei w, GLsizei h, GLsizei tex_w, GLsizei tex_h)
{
    if (g_sh.dev == nullptr || w <= 0 || h <= 0) {
        return;
    }
    flush_pending(c);
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    Texture *t = target_tex(c, target);
    int16_t r[4];
    win_rect_to_dev(c, x, y, w, h, r);
    int x0 = std::max<int>(0, r[0]), y0 = std::max<int>(0, r[1]);
    int x1 = std::min<int>(g_sh.dev_w, r[0] + r[2]), y1 = std::min<int>(g_sh.dev_h, r[1] + r[3]);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    if (t->resident) {
        device_delete(t);
    }
    ensure_dev_id(t);
    if (t->dev_id == 0) {
        return;
    }
    t->w = tex_w;
    t->h = tex_h;
    t->dw = x1 - x0;
    t->dh = y1 - y0;
    t->dev_fmt = ESPGPU_FMT_RGB565;
    t->dev_bytes = static_cast<uint32_t>(t->dw * t->dh * 2);
    make_room(t, t->dev_bytes);
    espgpu_fbo_t f{};
    f.action = ESPGPU_FBO_COPY_TO_TEXTURE;
    f.texture_id = t->dev_id;
    f.width = static_cast<uint16_t>(t->dw);
    f.height = static_cast<uint16_t>(t->dh);
    f.x = static_cast<uint16_t>(x0);
    f.y = static_cast<uint16_t>(y0);
    espgpu_cmd_fbo(g_sh.dev, &f);
    t->rgba.clear();
    t->index.clear();
    t->paletted = false;
    t->has_image = true;
    t->device_only = true;
    t->resident = true;
    t->dirty = false;
    t->upload = false;
    t->internal_format = GL_RGB;
    g_sh.tex_used += t->dev_bytes;
    g_sh.bind_epoch++;
    state_dirty(c);
}

void impl_glCopyTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width,
                           GLsizei height, GLint border)
{
    ESPGL_CTX(c);
    (void)internalFormat;
    (void)border;
    if (level != 0) {
        return;
    }
    copy_tex(c, target, x, y, width, height, width, height);
}

void impl_glCopyTexImage1D(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width,
                           GLint border)
{
    ESPGL_CTX(c);
    (void)internalFormat;
    (void)border;
    if (level != 0) {
        return;
    }
    copy_tex(c, target, x, y, width, 1, width, 1);
}

void impl_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                              GLsizei width, GLsizei height)
{
    ESPGL_CTX(c);
    if (level != 0) {
        return;
    }
    Texture *t = target_tex(c, target);
    /* 设备的 COPY 只能整张替换：按纹理整体尺寸取源矩形，使 (xoffset,yoffset) 对齐到源点 */
    const GLsizei tw = t->w > 0 ? t->w : width;
    const GLsizei th = t->h > 0 ? t->h : height;
    copy_tex(c, target, x - xoffset, y - yoffset, tw, th, tw, th);
}

void impl_glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width)
{
    impl_glCopyTexSubImage2D(target, level, xoffset, 0, x, y, width, 1);
}

/* ── 参数 ─────────────────────────────────────────────────────────── */

static void tex_param(Context *c, GLenum target, GLenum pname, const float *p)
{
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    Texture *t = target_tex(c, target);
    GLenum *field = nullptr;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: field = &t->min_filter; break;
    case GL_TEXTURE_MAG_FILTER: field = &t->mag_filter; break;
    case GL_TEXTURE_WRAP_S: field = &t->wrap_s; break;
    case GL_TEXTURE_WRAP_T: field = &t->wrap_t; break;
    default: break;
    }
    if (field) {
        const GLenum v = static_cast<GLenum>(p[0]);
        if (*field != v) {
            flush_if_used(c, t); /* 过滤 / 环绕是绑定参数：用到它的批次按旧值发 */
            *field = v;
        }
        return;
    }
    switch (pname) {
    case GL_TEXTURE_BORDER_COLOR: memcpy(t->border_color, p, 16); break;
    case GL_TEXTURE_PRIORITY:
        t->priority = p[0];
        t->prioritized = p[0] >= 1.0f;
        break;
    default: break; /* GENERATE_MIPMAP / MAX_ANISOTROPY / LOD 等：硬件不支持，忽略 */
    }
}

void impl_glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
    ESPGL_CTX(c);
    if (params == nullptr) {
        return;
    }
    float v[4] = {params[0], 0, 0, 0};
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        memcpy(v, params, 16);
    }
    ESPGL_RECORD(c, impl_glTexParameterfv(target, pname, v));
    tex_param(c, target, pname, v);
}

void impl_glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    impl_glTexParameterfv(target, pname, &param);
}

void impl_glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    const GLfloat f = static_cast<GLfloat>(param);
    impl_glTexParameterfv(target, pname, &f);
}

void impl_glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
    if (params == nullptr) {
        return;
    }
    GLfloat f[4] = {static_cast<GLfloat>(params[0]), 0, 0, 0};
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        for (int i = 0; i < 4; i++) f[i] = static_cast<GLfloat>(params[i]) / 2147483647.0f;
    }
    impl_glTexParameterfv(target, pname, f);
}

void impl_glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params)
{
    ESPGL_CTX(c);
    if (params == nullptr) {
        return;
    }
    Texture *t = target_tex(c, target);
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: params[0] = static_cast<GLfloat>(t->min_filter); break;
    case GL_TEXTURE_MAG_FILTER: params[0] = static_cast<GLfloat>(t->mag_filter); break;
    case GL_TEXTURE_WRAP_S: params[0] = static_cast<GLfloat>(t->wrap_s); break;
    case GL_TEXTURE_WRAP_T: params[0] = static_cast<GLfloat>(t->wrap_t); break;
    case GL_TEXTURE_BORDER_COLOR: memcpy(params, t->border_color, 16); break;
    case GL_TEXTURE_PRIORITY: params[0] = t->priority; break;
    case GL_TEXTURE_RESIDENT: params[0] = t->resident ? 1.0f : 0.0f; break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

void impl_glGetTexParameteriv(GLenum target, GLenum pname, GLint *params)
{
    GLfloat f[4] = {0, 0, 0, 0};
    impl_glGetTexParameterfv(target, pname, f);
    if (params) {
        const int n = pname == GL_TEXTURE_BORDER_COLOR ? 4 : 1;
        for (int i = 0; i < n; i++) params[i] = static_cast<GLint>(f[i]);
    }
}

void impl_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
    ESPGL_CTX(c);
    if (params == nullptr) {
        return;
    }
    int w = 0, h = 0;
    GLint ifmt = GL_RGBA;
    if (target == 0x8064 || target == 0x8063) {
        auto it = g_proxy.find(target);
        if (it != g_proxy.end()) {
            w = it->second.first;
            h = it->second.second;
        }
    } else {
        Texture *t = target_tex(c, target);
        if (level >= 0 && level < 12) {
            w = t->level_w[level];
            h = t->level_h[level];
        }
        if (level == 0) {
            w = t->w;
            h = t->h;
        }
        ifmt = t->internal_format;
    }
    switch (pname) {
    case GL_TEXTURE_WIDTH: params[0] = w; break;
    case GL_TEXTURE_HEIGHT: params[0] = h; break;
    case GL_TEXTURE_INTERNAL_FORMAT: params[0] = ifmt; break;
    case GL_TEXTURE_BORDER: params[0] = 0; break;
    case GL_TEXTURE_RED_SIZE:
    case GL_TEXTURE_BLUE_SIZE: params[0] = 5; break;
    case GL_TEXTURE_GREEN_SIZE: params[0] = 6; break;
    case GL_TEXTURE_ALPHA_SIZE: params[0] = 4; break;
    case GL_TEXTURE_LUMINANCE_SIZE:
    case GL_TEXTURE_INTENSITY_SIZE: params[0] = 8; break;
    default: params[0] = 0; break;
    }
}

void impl_glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params)
{
    GLint v = 0;
    impl_glGetTexLevelParameteriv(target, level, pname, &v);
    if (params) params[0] = static_cast<GLfloat>(v);
}

void impl_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels)
{
    ESPGL_CTX(c);
    if (level != 0 || pixels == nullptr) {
        return;
    }
    Texture *t = target_tex(c, target);
    if (!t->has_image || t->device_only) {
        return;
    }
    std::vector<uint8_t> rgba;
    const uint8_t *src = t->rgba.data();
    if (t->paletted) {
        rgba.resize(t->index.size() * 4);
        for (size_t i = 0; i < t->index.size(); i++) {
            const uint8_t *p = (t->has_palette ? t->palette : g_sh.shared_palette) + t->index[i] * 4;
            memcpy(&rgba[i * 4], p, 4);
        }
        src = rgba.data();
    }
    tex_pack_from_rgba(c, t->w, t->h, format, type, src, pixels);
}

/* ── 纹理环境 ─────────────────────────────────────────────────────── */

void impl_glTexEnvfv(GLenum target, GLenum pname, const GLfloat *p)
{
    ESPGL_CTX(c);
    if (p == nullptr) {
        return;
    }
    float v[4] = {p[0], 0, 0, 0};
    if (pname == GL_TEXTURE_ENV_COLOR) {
        memcpy(v, p, 16);
    }
    ESPGL_RECORD(c, impl_glTexEnvfv(target, pname, v));
    TexUnitState &u = c->st.unit[c->st.active_unit];
    if (target == 0x8500 /* GL_TEXTURE_FILTER_CONTROL */) {
        if (pname == 0x8501) u.lod_bias = v[0];
        return;
    }
    if (target != GL_TEXTURE_ENV) {
        return;
    }
    state_changed(c);
    const GLenum e = static_cast<GLenum>(v[0]);
    switch (pname) {
    case GL_TEXTURE_ENV_MODE: u.env_mode = e; break;
    case GL_TEXTURE_ENV_COLOR: memcpy(u.env_color, v, 16); break;
    case 0x8571: u.combine_rgb = e; break;
    case 0x8572: u.combine_alpha = e; break;
    case 0x8580: case 0x8581: case 0x8582: u.src_rgb[pname - 0x8580] = e; break;
    case 0x8588: case 0x8589: case 0x858A: u.src_alpha[pname - 0x8588] = e; break;
    case 0x8590: case 0x8591: case 0x8592: u.op_rgb[pname - 0x8590] = e; break;
    case 0x8598: case 0x8599: case 0x859A: u.op_alpha[pname - 0x8598] = e; break;
    case 0x8573: u.rgb_scale = v[0]; break;
    case 0x0D1C: u.alpha_scale = v[0]; break;
    default: break;
    }
}

void impl_glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    impl_glTexEnvfv(target, pname, &param);
}

void impl_glTexEnvi(GLenum target, GLenum pname, GLint param)
{
    const GLfloat f = static_cast<GLfloat>(param);
    impl_glTexEnvfv(target, pname, &f);
}

void impl_glTexEnviv(GLenum target, GLenum pname, const GLint *params)
{
    if (params == nullptr) {
        return;
    }
    GLfloat f[4] = {static_cast<GLfloat>(params[0]), 0, 0, 0};
    if (pname == GL_TEXTURE_ENV_COLOR) {
        for (int i = 0; i < 4; i++) f[i] = static_cast<GLfloat>(params[i]) / 2147483647.0f;
    }
    impl_glTexEnvfv(target, pname, f);
}

void impl_glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params)
{
    ESPGL_CTX(c);
    (void)target;
    if (params == nullptr) {
        return;
    }
    const TexUnitState &u = c->st.unit[c->st.active_unit];
    switch (pname) {
    case GL_TEXTURE_ENV_MODE: params[0] = static_cast<GLfloat>(u.env_mode); break;
    case GL_TEXTURE_ENV_COLOR: memcpy(params, u.env_color, 16); break;
    case 0x8571: params[0] = static_cast<GLfloat>(u.combine_rgb); break;
    case 0x8572: params[0] = static_cast<GLfloat>(u.combine_alpha); break;
    case 0x8573: params[0] = u.rgb_scale; break;
    case 0x0D1C: params[0] = u.alpha_scale; break;
    default:
        if (pname >= 0x8580 && pname <= 0x8582) params[0] = static_cast<GLfloat>(u.src_rgb[pname - 0x8580]);
        else if (pname >= 0x8588 && pname <= 0x858A) params[0] = static_cast<GLfloat>(u.src_alpha[pname - 0x8588]);
        else if (pname >= 0x8590 && pname <= 0x8592) params[0] = static_cast<GLfloat>(u.op_rgb[pname - 0x8590]);
        else if (pname >= 0x8598 && pname <= 0x859A) params[0] = static_cast<GLfloat>(u.op_alpha[pname - 0x8598]);
        break;
    }
}

void impl_glGetTexEnviv(GLenum target, GLenum pname, GLint *params)
{
    GLfloat f[4] = {0, 0, 0, 0};
    impl_glGetTexEnvfv(target, pname, f);
    if (params) {
        if (pname == GL_TEXTURE_ENV_COLOR) {
            for (int i = 0; i < 4; i++) params[i] = static_cast<GLint>(f[i] * 2147483647.0f);
        } else {
            params[0] = static_cast<GLint>(f[0]);
        }
    }
}

/* 纹理坐标生成：设备没有，接受参数但不生效（启用时记一次日志） */
void impl_glTexGeni(GLenum coord, GLenum pname, GLint param) { (void)coord; (void)pname; (void)param; }
void impl_glTexGenf(GLenum coord, GLenum pname, GLfloat param) { (void)coord; (void)pname; (void)param; }
void impl_glTexGend(GLenum coord, GLenum pname, GLdouble param) { (void)coord; (void)pname; (void)param; }
void impl_glTexGeniv(GLenum coord, GLenum pname, const GLint *params) { (void)coord; (void)pname; (void)params; }
void impl_glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params) { (void)coord; (void)pname; (void)params; }
void impl_glTexGendv(GLenum coord, GLenum pname, const GLdouble *params) { (void)coord; (void)pname; (void)params; }

/* ── 调色板（GL_EXT_paletted_texture / shared_texture_palette） ─────── */

static void color_table(Context *c, GLenum target, GLsizei start, GLsizei count, GLenum format, GLenum type,
                        const GLvoid *data, bool replace)
{
    if (data == nullptr || count <= 0) {
        return;
    }
    std::vector<uint8_t> rgba;
    tex_unpack_to_rgba(c, count, 1, format, type, data, rgba, nullptr);
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    {
        /* 内容没变（有的引擎每次绑定都重设一遍调色板）：什么都不做，免得整张纹理重传 */
        const bool shared = target == 0x81FB;
        Texture *t = shared ? nullptr : target_tex(c, target == 0x8064 ? GL_TEXTURE_2D : target);
        const uint8_t *cur_pal = shared ? g_sh.shared_palette : t->palette;
        const int cur_size = shared ? g_sh.shared_palette_size : t->palette_size;
        const int end = std::min(256, static_cast<int>(start + count));
        const bool has = shared ? true : (t->has_palette && t->palette_format != 0);
        if (has && start >= 0 && end <= cur_size && (!replace || end == cur_size) &&
            memcmp(cur_pal + start * 4, rgba.data(), static_cast<size_t>(end - start) * 4) == 0) {
            return;
        }
    }
    uint8_t *dst;
    int *size;
    if (target == 0x81FB /* GL_SHARED_TEXTURE_PALETTE_EXT */) {
        state_dirty(c);
        dst = g_sh.shared_palette;
        size = &g_sh.shared_palette_size;
        /* 共享调色板变了：所有没有自带调色板的 INDEX8 纹理都要重发调色板 */
        for (auto &kv : g_sh.textures) {
            if (kv.second->paletted && !kv.second->has_palette) {
                kv.second->upload = true;
                kv.second->pal_changed = true;
            }
        }
    } else {
        Texture *t = target_tex(c, target == 0x8064 ? GL_TEXTURE_2D : target);
        flush_if_used(c, t);
        dst = t->palette;
        size = &t->palette_size;
        t->has_palette = true;
        t->palette_format = format;
        t->upload = true;
        t->pal_changed = true;
    }
    for (GLsizei i = 0; i < count && start + i < 256; i++) {
        memcpy(dst + (start + i) * 4, &rgba[static_cast<size_t>(i) * 4], 4);
    }
    if (replace) {
        *size = std::min(256, static_cast<int>(start + count));
    } else {
        *size = std::max(*size, std::min(256, static_cast<int>(start + count)));
    }
}

void impl_glColorTable(GLenum target, GLenum internalformat, GLsizei width, GLenum format, GLenum type,
                       const GLvoid *table)
{
    ESPGL_CTX(c);
    (void)internalformat;
    color_table(c, target, 0, width, format, type, table, true);
}

void impl_glColorSubTable(GLenum target, GLsizei start, GLsizei count, GLenum format, GLenum type, const GLvoid *data)
{
    ESPGL_CTX(c);
    color_table(c, target, start, count, format, type, data, false);
}

void impl_glGetColorTable(GLenum target, GLenum format, GLenum type, GLvoid *table)
{
    ESPGL_CTX(c);
    const uint8_t *src;
    int n;
    if (target == 0x81FB) {
        src = g_sh.shared_palette;
        n = g_sh.shared_palette_size;
    } else {
        Texture *t = target_tex(c, target);
        src = t->palette;
        n = t->palette_size;
    }
    tex_pack_from_rgba(c, n, 1, format, type, src, table);
}

void impl_glGetColorTableParameteriv(GLenum target, GLenum pname, GLint *params)
{
    ESPGL_CTX(c);
    if (params == nullptr) {
        return;
    }
    int n = 0;
    if (target == 0x81FB) {
        n = g_sh.shared_palette_size;
    } else {
        n = target_tex(c, target)->palette_size;
    }
    switch (pname) {
    case 0x80D9 /* GL_COLOR_TABLE_WIDTH */: params[0] = n; break;
    case 0x80D8 /* GL_COLOR_TABLE_FORMAT */: params[0] = GL_RGBA; break;
    default: params[0] = 8; break; /* *_SIZE */
    }
}

void impl_glGetColorTableParameterfv(GLenum target, GLenum pname, GLfloat *params)
{
    GLint v = 0;
    impl_glGetColorTableParameteriv(target, pname, &v);
    if (params) params[0] = static_cast<GLfloat>(v);
}

} // namespace espgl
