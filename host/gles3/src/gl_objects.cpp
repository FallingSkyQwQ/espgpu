/*
 * gl_objects.cpp — GLES 3.0 入口：对象、状态、着色器 / 程序、uniform、查询
 *
 * 绘制、清屏、读回、SwapBuffers 在 gl_draw.cpp。
 * 不支持的调用设 GL_INVALID_OPERATION / GL_INVALID_ENUM 并只记一次日志。
 */

#include "context.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace gles;
namespace glsl = espgpu::glsl;

namespace gles {

namespace {
thread_local Context *t_current = nullptr;
uint32_t g_program_serial = 1;
} // namespace

Context *current() { return t_current; }
void set_current(Context *c) { t_current = c; }

void set_error(GLenum e)
{
    Context *c = t_current;
    if (c && c->error == GL_NO_ERROR) c->error = e;
}

void warn_once(const char *what)
{
    Context *c = t_current;
    if (!c || c->warned.count(what)) return;
    c->warned.insert(what);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[espgpu-gles] unsupported: %s\n", what);
    OutputDebugStringA(buf);
    std::fputs(buf, stderr);
}

/* ── 像素解包 → RGBA8888 ─────────────────────────────────────────── */

namespace {

float half_to_float(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
    return s ? -v : v;
}

uint8_t f2u8(float f)
{
    if (!(f > 0)) return 0;
    if (f >= 1) return 255;
    return static_cast<uint8_t>(f * 255.0f + 0.5f);
}

int format_channels(GLenum f)
{
    switch (f) {
    case GL_RGBA: case 0x80E1 /* GL_BGRA_EXT */: return 4;
    case GL_RGB: return 3;
    case GL_LUMINANCE_ALPHA: case GL_RG: return 2;
    case GL_LUMINANCE: case GL_ALPHA: case GL_RED: return 1;
    default: return 0;
    }
}

} // namespace

bool unpack_to_rgba(const Context &c, int w, int h, GLenum format, GLenum type, const void *src,
                    std::vector<uint8_t> &out, bool &has_alpha, bool &luminance)
{
    const int ch = format_channels(format);
    if (ch == 0) return false;
    int bpp;
    switch (type) {
    case GL_UNSIGNED_BYTE: bpp = ch; break;
    case GL_UNSIGNED_SHORT_5_6_5:
        if (format != GL_RGB) return false;
        bpp = 2;
        break;
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        if (format != GL_RGBA) return false;
        bpp = 2;
        break;
    case GL_FLOAT: bpp = ch * 4; break;
    case GL_HALF_FLOAT: case 0x8D61 /* GL_HALF_FLOAT_OES */: bpp = ch * 2; break;
    default: return false;
    }
    has_alpha = format == GL_RGBA || format == GL_LUMINANCE_ALPHA || format == GL_ALPHA || format == 0x80E1;
    luminance = format == GL_LUMINANCE;

    out.assign(static_cast<size_t>(w) * h * 4, 0);
    if (!src) {
        for (size_t i = 3; i < out.size(); i += 4) out[i] = has_alpha ? 0 : 255;
        return true;
    }
    const int row_px = c.unpack_row_length > 0 ? c.unpack_row_length : w;
    const int align = std::max(1, c.unpack_alignment);
    const size_t row_bytes = (static_cast<size_t>(row_px) * bpp + align - 1) / align * align;
    const auto *base = static_cast<const uint8_t *>(src) + static_cast<size_t>(c.unpack_skip_rows) * row_bytes +
                       static_cast<size_t>(c.unpack_skip_pixels) * bpp;

    for (int y = 0; y < h; y++) {
        const uint8_t *row = base + static_cast<size_t>(y) * row_bytes;
        for (int x = 0; x < w; x++) {
            float v[4] = {0, 0, 0, 1};
            const uint8_t *p = row + static_cast<size_t>(x) * bpp;
            if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
                type == GL_UNSIGNED_SHORT_5_5_5_1) {
                uint16_t s;
                std::memcpy(&s, p, 2);
                if (type == GL_UNSIGNED_SHORT_5_6_5) {
                    v[0] = ((s >> 11) & 31) / 31.0f;
                    v[1] = ((s >> 5) & 63) / 63.0f;
                    v[2] = (s & 31) / 31.0f;
                } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
                    v[0] = ((s >> 12) & 15) / 15.0f;
                    v[1] = ((s >> 8) & 15) / 15.0f;
                    v[2] = ((s >> 4) & 15) / 15.0f;
                    v[3] = (s & 15) / 15.0f;
                } else {
                    v[0] = ((s >> 11) & 31) / 31.0f;
                    v[1] = ((s >> 6) & 31) / 31.0f;
                    v[2] = ((s >> 1) & 31) / 31.0f;
                    v[3] = static_cast<float>(s & 1);
                }
            } else {
                float cc[4] = {0, 0, 0, 1};
                for (int k = 0; k < ch; k++) {
                    if (type == GL_UNSIGNED_BYTE) {
                        cc[k] = p[k] / 255.0f;
                    } else if (type == GL_FLOAT) {
                        std::memcpy(&cc[k], p + k * 4, 4);
                    } else {
                        uint16_t hv;
                        std::memcpy(&hv, p + k * 2, 2);
                        cc[k] = half_to_float(hv);
                    }
                }
                switch (format) {
                case GL_RGBA: v[0] = cc[0]; v[1] = cc[1]; v[2] = cc[2]; v[3] = cc[3]; break;
                case 0x80E1: v[0] = cc[2]; v[1] = cc[1]; v[2] = cc[0]; v[3] = cc[3]; break;
                case GL_RGB: v[0] = cc[0]; v[1] = cc[1]; v[2] = cc[2]; break;
                case GL_LUMINANCE: v[0] = v[1] = v[2] = cc[0]; break;
                case GL_LUMINANCE_ALPHA: v[0] = v[1] = v[2] = cc[0]; v[3] = cc[1]; break;
                case GL_ALPHA: v[3] = cc[0]; break;
                case GL_RED: v[0] = cc[0]; break;
                case GL_RG: v[0] = cc[0]; v[1] = cc[1]; break;
                default: break;
                }
            }
            uint8_t *o = &out[(static_cast<size_t>(y) * w + x) * 4];
            for (int k = 0; k < 4; k++) o[k] = f2u8(v[k]);
        }
    }
    return true;
}

} // namespace gles

/* ── 辅助 ─────────────────────────────────────────────────────────── */

namespace {

Buffer *bound_buffer(Context *c, GLenum target)
{
    GLuint id = 0;
    switch (target) {
    case GL_ARRAY_BUFFER: id = c->array_buffer; break;
    case GL_ELEMENT_ARRAY_BUFFER: id = c->cur_vao().element_buffer; break;
    case GL_COPY_READ_BUFFER: id = c->copy_read_buffer; break;
    case GL_COPY_WRITE_BUFFER: id = c->copy_write_buffer; break;
    case GL_PIXEL_PACK_BUFFER: id = c->pixel_pack_buffer; break;
    case GL_PIXEL_UNPACK_BUFFER: id = c->pixel_unpack_buffer; break;
    default: set_error(GL_INVALID_ENUM); return nullptr;
    }
    if (id == 0) {
        set_error(GL_INVALID_OPERATION);
        return nullptr;
    }
    auto it = c->sh->buffers.find(id);
    return it == c->sh->buffers.end() ? nullptr : &it->second;
}

Texture *bound_texture(Context *c, GLenum target)
{
    if (target != GL_TEXTURE_2D) {
        warn_once("texture targets other than GL_TEXTURE_2D");
        set_error(GL_INVALID_ENUM);
        return nullptr;
    }
    const GLuint id = c->tex2d[c->active_texture];
    if (id == 0) {
        set_error(GL_INVALID_OPERATION);
        return nullptr;
    }
    return &c->sh->textures[id];
}

Program *get_program(Context *c, GLuint id, bool err = true)
{
    auto it = c->sh->programs.find(id);
    if (it == c->sh->programs.end()) {
        if (err) set_error(c->sh->shaders.count(id) ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return nullptr;
    }
    return it->second.get();
}

Shader *get_shader(Context *c, GLuint id, bool err = true)
{
    auto it = c->sh->shaders.find(id);
    if (it == c->sh->shaders.end()) {
        if (err) set_error(c->sh->programs.count(id) ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return nullptr;
    }
    return it->second.get();
}

void copy_str(const std::string &s, GLsizei buf, GLsizei *len, GLchar *out)
{
    if (buf <= 0 || !out) {
        if (len) *len = 0;
        return;
    }
    const GLsizei n = std::min<GLsizei>(buf - 1, static_cast<GLsizei>(s.size()));
    std::memcpy(out, s.data(), static_cast<size_t>(n));
    out[n] = 0;
    if (len) *len = n;
}

void maybe_delete_shader(Context *c, GLuint id)
{
    auto it = c->sh->shaders.find(id);
    if (it != c->sh->shaders.end() && it->second->delete_pending && it->second->attach_count <= 0) {
        c->sh->shaders.erase(it);
    }
}

/** uniform 写入。comps = 每元素分量数；kind 0=float 1=int 2=uint；mat_cols/rows > 0 表示矩阵 */
void set_uniform(GLint loc, int comps, int kind, GLsizei count, const void *data, int mat_cols = 0,
                 int mat_rows = 0, bool transpose = false)
{
    GLES_CTX();
    if (loc == -1) return;
    Program *p = ctx->program ? get_program(ctx, ctx->program, false) : nullptr;
    if (!p || !p->linked) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    if (loc < 0 || loc >= static_cast<GLint>(p->locations.size()) || count < 0) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    const UniformLoc ul = p->locations[static_cast<size_t>(loc)];
    const glsl::ProgramUniform &u = p->link.uniforms[static_cast<size_t>(ul.uniform)];

    const bool is_mat = mat_cols > 0;
    const bool u_mat = u.columns > 1;
    if (is_mat != u_mat || (is_mat && (mat_cols != u.columns || mat_rows != u.components))) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    if (!is_mat && comps != u.components) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    const bool is_bool = u.gl_type == glsl::GLT_BOOL || u.gl_type == glsl::GLT_BOOL_VEC2 ||
                         u.gl_type == glsl::GLT_BOOL_VEC3 || u.gl_type == glsl::GLT_BOOL_VEC4;
    if (u.is_sampler) {
        if (kind != 1 || comps != 1) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
    } else if (!is_bool) {
        const bool want_int = u.is_int;
        if (want_int != (kind != 0)) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
    }
    if (count > 1 && u.array_size == 1) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    const int cols = u.is_sampler ? 1 : u.columns;
    for (GLsizei k = 0; k < count; k++) {
        const int elem = ul.element + k;
        if (elem >= u.array_size) break;
        for (int c = 0; c < cols; c++) {
            auto &slot = p->storage[static_cast<size_t>(u.storage + elem * cols + c)];
            for (int r = 0; r < (is_mat ? mat_rows : comps); r++) {
                float v;
                const size_t idx = is_mat ? static_cast<size_t>(k) * mat_cols * mat_rows +
                                                (transpose ? static_cast<size_t>(r) * mat_cols + c
                                                           : static_cast<size_t>(c) * mat_rows + r)
                                          : static_cast<size_t>(k) * comps + r;
                if (kind == 0) v = static_cast<const float *>(data)[idx];
                else if (kind == 1) v = static_cast<float>(static_cast<const GLint *>(data)[idx]);
                else v = static_cast<float>(static_cast<const GLuint *>(data)[idx]);
                if (is_bool) v = v != 0 ? 1.0f : 0.0f;
                slot[static_cast<size_t>(r)] = v;
            }
        }
    }
    p->storage_version++;
}

void enable_cap(GLenum cap, bool on)
{
    GLES_CTX();
    switch (cap) {
    case GL_BLEND: ctx->blend = on; break;
    case GL_DEPTH_TEST: ctx->depth_test = on; break;
    case GL_CULL_FACE: ctx->cull = on; break;
    case GL_SCISSOR_TEST: ctx->scissor_test = on; break;
    case GL_POLYGON_OFFSET_FILL: ctx->polygon_offset = on; break;
    case GL_DITHER: ctx->dither = on; break;
    case GL_STENCIL_TEST:
        ctx->stencil_test = on;
        if (on) warn_once("stencil test (ignored)");
        break;
    case GL_SAMPLE_ALPHA_TO_COVERAGE:
    case GL_SAMPLE_COVERAGE:
    case GL_RASTERIZER_DISCARD:
    case GL_PRIMITIVE_RESTART_FIXED_INDEX:
        if (on) warn_once("multisample / rasterizer-discard / primitive-restart enables (ignored)");
        break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

bool get_state(GLenum pname, double *v, int &n)
{
    Context *c = current();
    Device &d = device();
    n = 1;
    switch (pname) {
    case GL_ACTIVE_TEXTURE: v[0] = GL_TEXTURE0 + c->active_texture; return true;
    case GL_ARRAY_BUFFER_BINDING: v[0] = c->array_buffer; return true;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: v[0] = c->cur_vao().element_buffer; return true;
    case GL_VERTEX_ARRAY_BINDING: v[0] = c->vao; return true;
    case GL_CURRENT_PROGRAM: v[0] = c->program; return true;
    case GL_FRAMEBUFFER_BINDING: v[0] = c->draw_fbo; return true;
    case GL_READ_FRAMEBUFFER_BINDING: v[0] = c->read_fbo; return true;
    case GL_RENDERBUFFER_BINDING: v[0] = c->renderbuffer; return true;
    case GL_TEXTURE_BINDING_2D: v[0] = c->tex2d[c->active_texture]; return true;
    case GL_TEXTURE_BINDING_CUBE_MAP: case GL_TEXTURE_BINDING_3D: case GL_TEXTURE_BINDING_2D_ARRAY: v[0] = 0; return true;
    case GL_BLEND: v[0] = c->blend; return true;
    case GL_DEPTH_TEST: v[0] = c->depth_test; return true;
    case GL_CULL_FACE: v[0] = c->cull; return true;
    case GL_SCISSOR_TEST: v[0] = c->scissor_test; return true;
    case GL_STENCIL_TEST: v[0] = c->stencil_test; return true;
    case GL_DITHER: v[0] = c->dither; return true;
    case GL_POLYGON_OFFSET_FILL: v[0] = c->polygon_offset; return true;
    case GL_BLEND_SRC_RGB: v[0] = c->blend_src_rgb; return true;
    case GL_BLEND_DST_RGB: v[0] = c->blend_dst_rgb; return true;
    case GL_BLEND_SRC_ALPHA: v[0] = c->blend_src_a; return true;
    case GL_BLEND_DST_ALPHA: v[0] = c->blend_dst_a; return true;
    case GL_BLEND_EQUATION_RGB: v[0] = c->blend_eq_rgb; return true;
    case GL_BLEND_EQUATION_ALPHA: v[0] = c->blend_eq_a; return true;
    case GL_BLEND_COLOR: n = 4; for (int i = 0; i < 4; i++) v[i] = c->blend_color[i]; return true;
    case GL_DEPTH_FUNC: v[0] = c->depth_func; return true;
    case GL_DEPTH_WRITEMASK: v[0] = c->depth_mask; return true;
    case GL_DEPTH_RANGE: n = 2; v[0] = c->depth_near; v[1] = c->depth_far; return true;
    case GL_DEPTH_CLEAR_VALUE: v[0] = c->clear_depth; return true;
    case GL_COLOR_CLEAR_VALUE: n = 4; for (int i = 0; i < 4; i++) v[i] = c->clear_color[i]; return true;
    case GL_COLOR_WRITEMASK: n = 4; for (int i = 0; i < 4; i++) v[i] = c->color_mask[i]; return true;
    case GL_CULL_FACE_MODE: v[0] = c->cull_face; return true;
    case GL_FRONT_FACE: v[0] = c->front_face; return true;
    case GL_VIEWPORT: n = 4; for (int i = 0; i < 4; i++) v[i] = c->viewport[i]; return true;
    case GL_SCISSOR_BOX: n = 4; for (int i = 0; i < 4; i++) v[i] = c->scissor[i]; return true;
    case GL_LINE_WIDTH: v[0] = c->line_width; return true;
    case GL_POLYGON_OFFSET_FACTOR: v[0] = c->poly_factor; return true;
    case GL_POLYGON_OFFSET_UNITS: v[0] = c->poly_units; return true;
    case GL_UNPACK_ALIGNMENT: v[0] = c->unpack_alignment; return true;
    case GL_PACK_ALIGNMENT: v[0] = c->pack_alignment; return true;
    case GL_UNPACK_ROW_LENGTH: v[0] = c->unpack_row_length; return true;
    case GL_UNPACK_SKIP_ROWS: v[0] = c->unpack_skip_rows; return true;
    case GL_UNPACK_SKIP_PIXELS: v[0] = c->unpack_skip_pixels; return true;
    case GL_PACK_ROW_LENGTH: v[0] = c->pack_row_length; return true;
    case GL_PACK_SKIP_ROWS: v[0] = c->pack_skip_rows; return true;
    case GL_PACK_SKIP_PIXELS: v[0] = c->pack_skip_pixels; return true;
    case GL_GENERATE_MIPMAP_HINT: v[0] = c->hint_mipmap; return true;
    case GL_STENCIL_FUNC: case GL_STENCIL_BACK_FUNC: v[0] = c->stencil_func; return true;
    case GL_STENCIL_REF: case GL_STENCIL_BACK_REF: v[0] = c->stencil_ref; return true;
    case GL_STENCIL_VALUE_MASK: case GL_STENCIL_BACK_VALUE_MASK: v[0] = c->stencil_value_mask; return true;
    case GL_STENCIL_WRITEMASK: case GL_STENCIL_BACK_WRITEMASK: v[0] = c->stencil_writemask; return true;
    case GL_STENCIL_FAIL: case GL_STENCIL_BACK_FAIL: v[0] = c->stencil_fail; return true;
    case GL_STENCIL_PASS_DEPTH_FAIL: case GL_STENCIL_BACK_PASS_DEPTH_FAIL: v[0] = c->stencil_zfail; return true;
    case GL_STENCIL_PASS_DEPTH_PASS: case GL_STENCIL_BACK_PASS_DEPTH_PASS: v[0] = c->stencil_zpass; return true;
    case GL_STENCIL_CLEAR_VALUE: v[0] = c->clear_stencil; return true;
    case GL_READ_BUFFER: v[0] = c->read_buffer; return true;
    case GL_DRAW_BUFFER0: v[0] = c->draw_fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK; return true;
    case GL_COPY_READ_BUFFER: v[0] = c->copy_read_buffer; return true;
    case GL_COPY_WRITE_BUFFER: v[0] = c->copy_write_buffer; return true;
    case GL_PIXEL_PACK_BUFFER_BINDING: v[0] = c->pixel_pack_buffer; return true;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: v[0] = c->pixel_unpack_buffer; return true;
    /* 实现限制 */
    case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: v[0] = d.max_tex_dim; return true;
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: case GL_MAX_3D_TEXTURE_SIZE: v[0] = 0; return true;
    case GL_MAX_ARRAY_TEXTURE_LAYERS: v[0] = 1; return true;
    case GL_MAX_VIEWPORT_DIMS: n = 2; v[0] = v[1] = 4096; return true;
    case GL_MAX_VERTEX_ATTRIBS: v[0] = kMaxAttribs; return true;
    case GL_MAX_VERTEX_UNIFORM_VECTORS: v[0] = 256; return true;
    case GL_MAX_VERTEX_UNIFORM_COMPONENTS: v[0] = 1024; return true;
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS: v[0] = 24; return true;
    case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: v[0] = 96; return true;
    case GL_MAX_VARYING_VECTORS: v[0] = 6; return true;
    case GL_MAX_VARYING_COMPONENTS: v[0] = 24; return true;
    case GL_MAX_VERTEX_OUTPUT_COMPONENTS: case GL_MAX_FRAGMENT_INPUT_COMPONENTS: v[0] = 24; return true;
    case GL_MAX_TEXTURE_IMAGE_UNITS: v[0] = kDeviceTexUnits; return true;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: v[0] = kMaxTexUnits; return true;
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: v[0] = 0; return true;
    case GL_MAX_DRAW_BUFFERS: case GL_MAX_COLOR_ATTACHMENTS: v[0] = 1; return true;
    case GL_MAX_SAMPLES: v[0] = 0; return true;
    case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES: v[0] = 65536; return true;
    case GL_MAX_ELEMENT_INDEX: v[0] = 0xFFFFFFFFu; return true;
    case GL_MAX_TEXTURE_LOD_BIAS: v[0] = 0; return true;
    case GL_MIN_PROGRAM_TEXEL_OFFSET: case GL_MAX_PROGRAM_TEXEL_OFFSET: v[0] = 0; return true;
    case GL_MAX_UNIFORM_BUFFER_BINDINGS: case GL_MAX_VERTEX_UNIFORM_BLOCKS:
    case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: case GL_MAX_COMBINED_UNIFORM_BLOCKS: v[0] = 0; return true;
    case GL_MAX_UNIFORM_BLOCK_SIZE: v[0] = 0; return true;
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: v[0] = 4; return true;
    case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS: case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS:
    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS: v[0] = 0; return true;
    case GL_ALIASED_POINT_SIZE_RANGE: n = 2; v[0] = 1; v[1] = 16; return true;
    case GL_ALIASED_LINE_WIDTH_RANGE: n = 2; v[0] = 1; v[1] = 16; return true;
    case GL_SUBPIXEL_BITS: v[0] = 4; return true;
    case GL_RED_BITS: case GL_BLUE_BITS: v[0] = 5; return true;
    case GL_GREEN_BITS: v[0] = 6; return true;
    case GL_ALPHA_BITS: v[0] = 0; return true;
    case GL_DEPTH_BITS: v[0] = 16; return true;
    case GL_STENCIL_BITS: v[0] = 0; return true;
    case GL_SAMPLE_BUFFERS: case GL_SAMPLES: v[0] = 0; return true;
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: case GL_NUM_SHADER_BINARY_FORMATS:
    case GL_NUM_PROGRAM_BINARY_FORMATS: v[0] = 0; return true;
    case GL_COMPRESSED_TEXTURE_FORMATS: case GL_SHADER_BINARY_FORMATS: case GL_PROGRAM_BINARY_FORMATS: n = 0; return true;
    case GL_SHADER_COMPILER: v[0] = 1; return true;
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT: v[0] = GL_RGBA; return true;
    case GL_IMPLEMENTATION_COLOR_READ_TYPE: v[0] = GL_UNSIGNED_BYTE; return true;
    case GL_MAJOR_VERSION: v[0] = c->version >= 3 ? 3 : 2; return true;
    case GL_MINOR_VERSION: v[0] = 0; return true;
    case GL_NUM_EXTENSIONS: v[0] = 6; return true;
    case GL_MAX_SERVER_WAIT_TIMEOUT: v[0] = 0; return true;
    default: return false;
    }
}

const char *const kExtensions[] = {
    "GL_OES_rgb8_rgba8",       "GL_OES_element_index_uint", "GL_OES_vertex_array_object",
    "GL_OES_texture_npot",     "GL_EXT_texture_format_BGRA8888", "GL_OES_depth24",
};

} // namespace

/* ════════════════════════════════════════════════════════════════════
 * 入口
 * ════════════════════════════════════════════════════════════════════ */

extern "C" {

GL_APICALL GLenum GL_APIENTRY glGetError(void)
{
    GLES_CTX(GL_NO_ERROR);
    const GLenum e = ctx->error;
    ctx->error = GL_NO_ERROR;
    return e;
}

/* ── 缓冲 ── */

GL_APICALL void GL_APIENTRY glGenBuffers(GLsizei n, GLuint *buffers)
{
    GLES_CTX();
    if (n < 0) return set_error(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; i++) {
        while (ctx->sh->buffers.count(ctx->sh->next_buffer)) ctx->sh->next_buffer++;
        buffers[i] = ctx->sh->next_buffer++;
        ctx->sh->buffers[buffers[i]];
    }
}

GL_APICALL void GL_APIENTRY glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
    GLES_CTX();
    if (n < 0) return set_error(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; i++) {
        const GLuint id = buffers[i];
        if (!id) continue;
        ctx->sh->buffers.erase(id);
        if (ctx->array_buffer == id) ctx->array_buffer = 0;
        if (ctx->copy_read_buffer == id) ctx->copy_read_buffer = 0;
        if (ctx->copy_write_buffer == id) ctx->copy_write_buffer = 0;
        if (ctx->pixel_pack_buffer == id) ctx->pixel_pack_buffer = 0;
        if (ctx->pixel_unpack_buffer == id) ctx->pixel_unpack_buffer = 0;
        for (auto &kv : ctx->vaos) {
            if (kv.second.element_buffer == id) kv.second.element_buffer = 0;
            for (auto &a : kv.second.attribs) {
                if (a.buffer == id) a.buffer = 0;
            }
        }
    }
}

GL_APICALL GLboolean GL_APIENTRY glIsBuffer(GLuint buffer)
{
    GLES_CTX(GL_FALSE);
    return buffer && ctx->sh->buffers.count(buffer) ? GL_TRUE : GL_FALSE;
}

GL_APICALL void GL_APIENTRY glBindBuffer(GLenum target, GLuint buffer)
{
    GLES_CTX();
    if (buffer) ctx->sh->buffers[buffer];
    switch (target) {
    case GL_ARRAY_BUFFER: ctx->array_buffer = buffer; break;
    case GL_ELEMENT_ARRAY_BUFFER: ctx->cur_vao().element_buffer = buffer; break;
    case GL_COPY_READ_BUFFER: ctx->copy_read_buffer = buffer; break;
    case GL_COPY_WRITE_BUFFER: ctx->copy_write_buffer = buffer; break;
    case GL_PIXEL_PACK_BUFFER: ctx->pixel_pack_buffer = buffer; break;
    case GL_PIXEL_UNPACK_BUFFER: ctx->pixel_unpack_buffer = buffer; break;
    case GL_UNIFORM_BUFFER:
    case GL_TRANSFORM_FEEDBACK_BUFFER:
        warn_once("uniform / transform feedback buffers");
        break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

GL_APICALL void GL_APIENTRY glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    GLES_CTX();
    if (size < 0) return set_error(GL_INVALID_VALUE);
    Buffer *b = bound_buffer(ctx, target);
    if (!b) return;
    b->data.assign(static_cast<size_t>(size), 0);
    if (data) std::memcpy(b->data.data(), data, static_cast<size_t>(size));
    b->usage = usage;
    b->mapped = false;
}

GL_APICALL void GL_APIENTRY glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    GLES_CTX();
    Buffer *b = bound_buffer(ctx, target);
    if (!b) return;
    if (offset < 0 || size < 0 || static_cast<size_t>(offset + size) > b->data.size()) return set_error(GL_INVALID_VALUE);
    if (data && size) std::memcpy(b->data.data() + offset, data, static_cast<size_t>(size));
}

GL_APICALL void *GL_APIENTRY glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    GLES_CTX(nullptr);
    Buffer *b = bound_buffer(ctx, target);
    if (!b) return nullptr;
    if (offset < 0 || length < 0 || static_cast<size_t>(offset + length) > b->data.size() || b->mapped ||
        !(access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT))) {
        set_error(GL_INVALID_VALUE);
        return nullptr;
    }
    b->mapped = true;
    b->map_offset = offset;
    b->map_length = length;
    b->map_access = access;
    if (access & GL_MAP_INVALIDATE_BUFFER_BIT) std::fill(b->data.begin(), b->data.end(), 0);
    return b->data.data() + offset;
}

GL_APICALL GLboolean GL_APIENTRY glUnmapBuffer(GLenum target)
{
    GLES_CTX(GL_FALSE);
    Buffer *b = bound_buffer(ctx, target);
    if (!b) return GL_FALSE;
    if (!b->mapped) {
        set_error(GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    b->mapped = false;
    return GL_TRUE;
}

GL_APICALL void GL_APIENTRY glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length)
{
    (void)target;
    (void)offset;
    (void)length; /* 映射的就是主机内存，无需刷新 */
}

GL_APICALL void GL_APIENTRY glGetBufferPointerv(GLenum target, GLenum pname, void **params)
{
    GLES_CTX();
    Buffer *b = bound_buffer(ctx, target);
    if (!b || pname != GL_BUFFER_MAP_POINTER) return;
    *params = b->mapped ? b->data.data() + b->map_offset : nullptr;
}

GL_APICALL void GL_APIENTRY glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset,
                                                GLintptr writeOffset, GLsizeiptr size)
{
    GLES_CTX();
    Buffer *r = bound_buffer(ctx, readTarget);
    Buffer *w = bound_buffer(ctx, writeTarget);
    if (!r || !w) return;
    if (readOffset < 0 || writeOffset < 0 || size < 0 || static_cast<size_t>(readOffset + size) > r->data.size() ||
        static_cast<size_t>(writeOffset + size) > w->data.size()) {
        return set_error(GL_INVALID_VALUE);
    }
    std::memmove(w->data.data() + writeOffset, r->data.data() + readOffset, static_cast<size_t>(size));
}

GL_APICALL void GL_APIENTRY glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params)
{
    GLES_CTX();
    Buffer *b = bound_buffer(ctx, target);
    if (!b) return;
    switch (pname) {
    case GL_BUFFER_SIZE: *params = static_cast<GLint>(b->data.size()); break;
    case GL_BUFFER_USAGE: *params = static_cast<GLint>(b->usage); break;
    case GL_BUFFER_MAPPED: *params = b->mapped; break;
    case GL_BUFFER_ACCESS_FLAGS: *params = static_cast<GLint>(b->map_access); break;
    case GL_BUFFER_MAP_LENGTH: *params = static_cast<GLint>(b->map_length); break;
    case GL_BUFFER_MAP_OFFSET: *params = static_cast<GLint>(b->map_offset); break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

/* ── 顶点数组 ── */

GL_APICALL void GL_APIENTRY glGenVertexArrays(GLsizei n, GLuint *arrays)
{
    GLES_CTX();
    if (n < 0) return set_error(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; i++) {
        while (ctx->vaos.count(ctx->next_vao)) ctx->next_vao++;
        arrays[i] = ctx->next_vao++;
        ctx->vaos[arrays[i]] = VertexArray{};
    }
}

GL_APICALL void GL_APIENTRY glDeleteVertexArrays(GLsizei n, const GLuint *arrays)
{
    GLES_CTX();
    for (GLsizei i = 0; i < n; i++) {
        if (!arrays[i]) continue;
        ctx->vaos.erase(arrays[i]);
        if (ctx->vao == arrays[i]) ctx->vao = 0;
    }
}

GL_APICALL GLboolean GL_APIENTRY glIsVertexArray(GLuint array)
{
    GLES_CTX(GL_FALSE);
    return array && ctx->vaos.count(array) ? GL_TRUE : GL_FALSE;
}

GL_APICALL void GL_APIENTRY glBindVertexArray(GLuint array)
{
    GLES_CTX();
    if (array && !ctx->vaos.count(array)) return set_error(GL_INVALID_OPERATION);
    ctx->vao = array;
}

GL_APICALL void GL_APIENTRY glEnableVertexAttribArray(GLuint index)
{
    GLES_CTX();
    if (index >= kMaxAttribs) return set_error(GL_INVALID_VALUE);
    ctx->cur_vao().attribs[index].enabled = true;
}

GL_APICALL void GL_APIENTRY glDisableVertexAttribArray(GLuint index)
{
    GLES_CTX();
    if (index >= kMaxAttribs) return set_error(GL_INVALID_VALUE);
    ctx->cur_vao().attribs[index].enabled = false;
}

static void attrib_pointer(GLuint index, GLint size, GLenum type, bool normalized, bool integer, GLsizei stride,
                           const void *pointer)
{
    GLES_CTX();
    if (index >= kMaxAttribs || size < 1 || size > 4 || stride < 0) return set_error(GL_INVALID_VALUE);
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_INT:
    case GL_UNSIGNED_INT: break;
    case GL_FLOAT: case GL_HALF_FLOAT: case GL_FIXED: case 0x8D61:
        if (integer) return set_error(GL_INVALID_ENUM);
        break;
    default: return set_error(GL_INVALID_ENUM);
    }
    AttribArray &a = ctx->cur_vao().attribs[index];
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.integer = integer;
    a.stride = stride;
    a.pointer = pointer;
    a.buffer = ctx->array_buffer;
}

GL_APICALL void GL_APIENTRY glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                                  GLsizei stride, const void *pointer)
{
    attrib_pointer(index, size, type, normalized != 0, false, stride, pointer);
}

GL_APICALL void GL_APIENTRY glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride,
                                                   const void *pointer)
{
    attrib_pointer(index, size, type, false, true, stride, pointer);
}

GL_APICALL void GL_APIENTRY glVertexAttribDivisor(GLuint index, GLuint divisor)
{
    GLES_CTX();
    if (index >= kMaxAttribs) return set_error(GL_INVALID_VALUE);
    ctx->cur_vao().attribs[index].divisor = divisor;
    if (divisor) warn_once("instanced attributes (divisor != 0)");
}

static void generic(GLuint index, float x, float y, float z, float w)
{
    GLES_CTX();
    if (index >= kMaxAttribs) return set_error(GL_INVALID_VALUE);
    ctx->generic_attr[index] = {x, y, z, w};
}

GL_APICALL void GL_APIENTRY glVertexAttrib1f(GLuint i, GLfloat x) { generic(i, x, 0, 0, 1); }
GL_APICALL void GL_APIENTRY glVertexAttrib2f(GLuint i, GLfloat x, GLfloat y) { generic(i, x, y, 0, 1); }
GL_APICALL void GL_APIENTRY glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z) { generic(i, x, y, z, 1); }
GL_APICALL void GL_APIENTRY glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { generic(i, x, y, z, w); }
GL_APICALL void GL_APIENTRY glVertexAttrib1fv(GLuint i, const GLfloat *v) { generic(i, v[0], 0, 0, 1); }
GL_APICALL void GL_APIENTRY glVertexAttrib2fv(GLuint i, const GLfloat *v) { generic(i, v[0], v[1], 0, 1); }
GL_APICALL void GL_APIENTRY glVertexAttrib3fv(GLuint i, const GLfloat *v) { generic(i, v[0], v[1], v[2], 1); }
GL_APICALL void GL_APIENTRY glVertexAttrib4fv(GLuint i, const GLfloat *v) { generic(i, v[0], v[1], v[2], v[3]); }
GL_APICALL void GL_APIENTRY glVertexAttribI4i(GLuint i, GLint x, GLint y, GLint z, GLint w)
{
    generic(i, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w));
}
GL_APICALL void GL_APIENTRY glVertexAttribI4ui(GLuint i, GLuint x, GLuint y, GLuint z, GLuint w)
{
    generic(i, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w));
}
GL_APICALL void GL_APIENTRY glVertexAttribI4iv(GLuint i, const GLint *v) { glVertexAttribI4i(i, v[0], v[1], v[2], v[3]); }
GL_APICALL void GL_APIENTRY glVertexAttribI4uiv(GLuint i, const GLuint *v) { glVertexAttribI4ui(i, v[0], v[1], v[2], v[3]); }

GL_APICALL void GL_APIENTRY glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params)
{
    GLES_CTX();
    if (index >= kMaxAttribs) return set_error(GL_INVALID_VALUE);
    const AttribArray &a = ctx->cur_vao().attribs[index];
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED: *params = a.enabled; break;
    case GL_VERTEX_ATTRIB_ARRAY_SIZE: *params = a.size; break;
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE: *params = a.stride; break;
    case GL_VERTEX_ATTRIB_ARRAY_TYPE: *params = static_cast<GLint>(a.type); break;
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: *params = a.normalized; break;
    case GL_VERTEX_ATTRIB_ARRAY_INTEGER: *params = a.integer; break;
    case GL_VERTEX_ATTRIB_ARRAY_DIVISOR: *params = static_cast<GLint>(a.divisor); break;
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: *params = static_cast<GLint>(a.buffer); break;
    case GL_CURRENT_VERTEX_ATTRIB:
        for (int i = 0; i < 4; i++) params[i] = static_cast<GLint>(ctx->generic_attr[index][static_cast<size_t>(i)]);
        break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

GL_APICALL void GL_APIENTRY glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params)
{
    GLES_CTX();
    if (index >= kMaxAttribs) return set_error(GL_INVALID_VALUE);
    if (pname == GL_CURRENT_VERTEX_ATTRIB) {
        for (int i = 0; i < 4; i++) params[i] = ctx->generic_attr[index][static_cast<size_t>(i)];
        return;
    }
    GLint v = 0;
    glGetVertexAttribiv(index, pname, &v);
    *params = static_cast<GLfloat>(v);
}

GL_APICALL void GL_APIENTRY glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer)
{
    GLES_CTX();
    if (index >= kMaxAttribs || pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) return set_error(GL_INVALID_ENUM);
    *pointer = const_cast<void *>(ctx->cur_vao().attribs[index].pointer);
}

/* ── 纹理 ── */

GL_APICALL void GL_APIENTRY glActiveTexture(GLenum texture)
{
    GLES_CTX();
    if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + kMaxTexUnits) return set_error(GL_INVALID_ENUM);
    ctx->active_texture = static_cast<int>(texture - GL_TEXTURE0);
}

GL_APICALL void GL_APIENTRY glGenTextures(GLsizei n, GLuint *textures)
{
    GLES_CTX();
    if (n < 0) return set_error(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; i++) {
        /* 纹理名直接当设备纹理 id（BIND_TEXTURE 里是 16 位），避开 0xC000 以上的保留区 */
        while (ctx->sh->textures.count(ctx->sh->next_texture)) ctx->sh->next_texture++;
        if (ctx->sh->next_texture >= 0xC000) {
            set_error(GL_OUT_OF_MEMORY);
            return;
        }
        textures[i] = ctx->sh->next_texture++;
        ctx->sh->textures[textures[i]];
    }
}

GL_APICALL void GL_APIENTRY glDeleteTextures(GLsizei n, const GLuint *textures)
{
    GLES_CTX();
    if (n < 0) return set_error(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; i++) {
        const GLuint id = textures[i];
        if (!id || !ctx->sh->textures.count(id)) continue;
        texture_deleted(id);
        ctx->sh->textures.erase(id);
        for (auto &t : ctx->tex2d) {
            if (t == id) t = 0;
        }
        for (auto &kv : ctx->fbos) {
            if (kv.second.color_tex == id) kv.second.color_tex = 0;
            if (kv.second.depth_tex == id) kv.second.depth_tex = 0;
        }
    }
}

GL_APICALL GLboolean GL_APIENTRY glIsTexture(GLuint texture)
{
    GLES_CTX(GL_FALSE);
    auto it = ctx->sh->textures.find(texture);
    return texture && it != ctx->sh->textures.end() && it->second.target != 0 ? GL_TRUE : GL_FALSE;
}

GL_APICALL void GL_APIENTRY glBindTexture(GLenum target, GLuint texture)
{
    GLES_CTX();
    if (target != GL_TEXTURE_2D) {
        if (target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY) {
            warn_once("cube map / 3D / array textures");
            return;
        }
        return set_error(GL_INVALID_ENUM);
    }
    if (texture) {
        if (texture >= 0xC000) return set_error(GL_INVALID_OPERATION);
        Texture &t = ctx->sh->textures[texture];
        if (t.target && t.target != target) return set_error(GL_INVALID_OPERATION);
        t.target = target;
    }
    ctx->tex2d[ctx->active_texture] = texture;
}

static void tex_image(Texture &t, int w, int h, GLenum internal_format, GLenum format, GLenum type, const void *pixels)
{
    GLES_CTX();
    const void *src = pixels;
    if (ctx->pixel_unpack_buffer) {
        auto it = ctx->sh->buffers.find(ctx->pixel_unpack_buffer);
        if (it == ctx->sh->buffers.end()) return set_error(GL_INVALID_OPERATION);
        src = it->second.data.data() + reinterpret_cast<uintptr_t>(pixels);
    }
    std::vector<uint8_t> rgba;
    bool alpha = true, lum = false;
    if (!unpack_to_rgba(*ctx, w, h, format, type, src, rgba, alpha, lum)) {
        warn_once("texture format/type combination");
        return set_error(GL_INVALID_ENUM);
    }
    t.device_only = false;
    t.width = w;
    t.height = h;
    t.internal_format = internal_format;
    t.rgba = std::move(rgba);
    t.has_alpha = alpha;
    t.luminance = lum;
    t.dirty = true;
}

GL_APICALL void GL_APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                         GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
{
    GLES_CTX();
    if (target != GL_TEXTURE_2D) {
        warn_once("glTexImage2D on non-2D targets");
        return set_error(GL_INVALID_ENUM);
    }
    if (level < 0 || width < 0 || height < 0 || border != 0) return set_error(GL_INVALID_VALUE);
    Texture *t = bound_texture(ctx, target);
    if (!t) return;
    if (t->immutable) return set_error(GL_INVALID_OPERATION);
    if (level > 0) return; /* 设备没有 mipmap：只保留第 0 级 */
    tex_image(*t, width, height, static_cast<GLenum>(internalformat), format, type, pixels);
}

GL_APICALL void GL_APIENTRY glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                                           GLsizei height)
{
    GLES_CTX();
    Texture *t = bound_texture(ctx, target);
    if (!t) return;
    if (levels < 1 || width < 1 || height < 1) return set_error(GL_INVALID_VALUE);
    if (t->immutable) return set_error(GL_INVALID_OPERATION);
    GLenum fmt = GL_RGBA;
    switch (internalformat) {
    case GL_RGB8: case GL_RGB565: case GL_SRGB8: case GL_RGB: fmt = GL_RGB; break;
    case GL_R8: fmt = GL_RED; break;
    case GL_RG8: fmt = GL_RG; break;
    case GL_LUMINANCE: fmt = GL_LUMINANCE; break;
    case GL_ALPHA: fmt = GL_ALPHA; break;
    case GL_LUMINANCE_ALPHA: fmt = GL_LUMINANCE_ALPHA; break;
    default: fmt = GL_RGBA; break;
    }
    const GLint save_rl = ctx->unpack_row_length;
    tex_image(*t, width, height, internalformat, fmt, GL_UNSIGNED_BYTE, nullptr);
    ctx->unpack_row_length = save_rl;
    t->immutable = true;
    t->levels = levels;
}

GL_APICALL void GL_APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                            GLsizei height, GLenum format, GLenum type, const void *pixels)
{
    GLES_CTX();
    Texture *t = bound_texture(ctx, target);
    if (!t) return;
    if (level > 0) return;
    if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 || xoffset + width > t->width ||
        yoffset + height > t->height) {
        return set_error(GL_INVALID_VALUE);
    }
    const void *src = pixels;
    if (ctx->pixel_unpack_buffer) {
        auto it = ctx->sh->buffers.find(ctx->pixel_unpack_buffer);
        if (it == ctx->sh->buffers.end()) return set_error(GL_INVALID_OPERATION);
        src = it->second.data.data() + reinterpret_cast<uintptr_t>(pixels);
    }
    std::vector<uint8_t> sub;
    bool alpha, lum;
    if (!unpack_to_rgba(*ctx, width, height, format, type, src, sub, alpha, lum)) return set_error(GL_INVALID_ENUM);
    if (t->rgba.size() != static_cast<size_t>(t->width) * t->height * 4) {
        t->rgba.assign(static_cast<size_t>(t->width) * t->height * 4, 0);
    }
    for (int y = 0; y < height; y++) {
        std::memcpy(&t->rgba[(static_cast<size_t>(yoffset + y) * t->width + xoffset) * 4],
                    &sub[static_cast<size_t>(y) * width * 4], static_cast<size_t>(width) * 4);
    }
    t->device_only = false;
    t->dirty = true;
}

GL_APICALL void GL_APIENTRY glCompressedTexImage2D(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)
{
    warn_once("compressed textures");
    set_error(GL_INVALID_ENUM);
}

GL_APICALL void GL_APIENTRY glCompressedTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei,
                                                      const void *)
{
    warn_once("compressed textures");
    set_error(GL_INVALID_ENUM);
}

GL_APICALL void GL_APIENTRY glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
                                             GLsizei width, GLsizei height, GLint border)
{
    GLES_CTX();
    Texture *t = bound_texture(ctx, target);
    if (!t) return;
    if (level > 0) return;
    if (border != 0 || width < 0 || height < 0) return set_error(GL_INVALID_VALUE);
    t->width = width;
    t->height = height;
    t->internal_format = internalformat;
    t->rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    copy_tex_image(ctx->tex2d[ctx->active_texture], x, y, width, height);
}

GL_APICALL void GL_APIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x,
                                                GLint y, GLsizei width, GLsizei height)
{
    GLES_CTX();
    Texture *t = bound_texture(ctx, target);
    if (!t) return;
    if (level > 0) return;
    if (xoffset != 0 || yoffset != 0 || width != t->width || height != t->height) {
        warn_once("glCopyTexSubImage2D into a sub-rectangle (whole texture is replaced)");
    }
    copy_tex_image(ctx->tex2d[ctx->active_texture], x, y, t->width, t->height);
}

GL_APICALL void GL_APIENTRY glGenerateMipmap(GLenum target)
{
    GLES_CTX();
    if (target != GL_TEXTURE_2D) return set_error(GL_INVALID_ENUM);
    /* 设备没有 mipmap，什么也不做 */
}

static void tex_param(GLenum target, GLenum pname, GLint v)
{
    GLES_CTX();
    Texture *t = bound_texture(ctx, target);
    if (!t) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->min_filter = static_cast<GLenum>(v); break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = static_cast<GLenum>(v); break;
    case GL_TEXTURE_WRAP_S: t->wrap_s = static_cast<GLenum>(v); break;
    case GL_TEXTURE_WRAP_T: t->wrap_t = static_cast<GLenum>(v); break;
    case GL_TEXTURE_WRAP_R: case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_MAX_LOD: case GL_TEXTURE_BASE_LEVEL:
    case GL_TEXTURE_MAX_LEVEL: case GL_TEXTURE_COMPARE_MODE: case GL_TEXTURE_COMPARE_FUNC:
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
    case 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */:
        break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

GL_APICALL void GL_APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param) { tex_param(target, pname, param); }
GL_APICALL void GL_APIENTRY glTexParameterf(GLenum target, GLenum pname, GLfloat param) { tex_param(target, pname, static_cast<GLint>(param)); }
GL_APICALL void GL_APIENTRY glTexParameteriv(GLenum target, GLenum pname, const GLint *params) { tex_param(target, pname, params[0]); }
GL_APICALL void GL_APIENTRY glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) { tex_param(target, pname, static_cast<GLint>(params[0])); }

GL_APICALL void GL_APIENTRY glGetTexParameteriv(GLenum target, GLenum pname, GLint *params)
{
    GLES_CTX();
    Texture *t = bound_texture(ctx, target);
    if (!t) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: *params = static_cast<GLint>(t->min_filter); break;
    case GL_TEXTURE_MAG_FILTER: *params = static_cast<GLint>(t->mag_filter); break;
    case GL_TEXTURE_WRAP_S: *params = static_cast<GLint>(t->wrap_s); break;
    case GL_TEXTURE_WRAP_T: *params = static_cast<GLint>(t->wrap_t); break;
    case GL_TEXTURE_IMMUTABLE_FORMAT: *params = t->immutable; break;
    case GL_TEXTURE_IMMUTABLE_LEVELS: *params = t->immutable ? t->levels : 0; break;
    case GL_TEXTURE_BASE_LEVEL: *params = 0; break;
    case GL_TEXTURE_MAX_LEVEL: *params = 1000; break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

GL_APICALL void GL_APIENTRY glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params)
{
    GLint v = 0;
    glGetTexParameteriv(target, pname, &v);
    *params = static_cast<GLfloat>(v);
}

GL_APICALL void GL_APIENTRY glPixelStorei(GLenum pname, GLint param)
{
    GLES_CTX();
    switch (pname) {
    case GL_UNPACK_ALIGNMENT:
    case GL_PACK_ALIGNMENT:
        if (param != 1 && param != 2 && param != 4 && param != 8) return set_error(GL_INVALID_VALUE);
        (pname == GL_UNPACK_ALIGNMENT ? ctx->unpack_alignment : ctx->pack_alignment) = param;
        break;
    case GL_UNPACK_ROW_LENGTH: ctx->unpack_row_length = param; break;
    case GL_UNPACK_SKIP_ROWS: ctx->unpack_skip_rows = param; break;
    case GL_UNPACK_SKIP_PIXELS: ctx->unpack_skip_pixels = param; break;
    case GL_PACK_ROW_LENGTH: ctx->pack_row_length = param; break;
    case GL_PACK_SKIP_ROWS: ctx->pack_skip_rows = param; break;
    case GL_PACK_SKIP_PIXELS: ctx->pack_skip_pixels = param; break;
    case GL_UNPACK_IMAGE_HEIGHT: case GL_UNPACK_SKIP_IMAGES: break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

/* ── 渲染缓冲 / 帧缓冲 ── */

GL_APICALL void GL_APIENTRY glGenRenderbuffers(GLsizei n, GLuint *rbs)
{
    GLES_CTX();
    if (n < 0) return set_error(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; i++) {
        while (ctx->sh->renderbuffers.count(ctx->sh->next_rb)) ctx->sh->next_rb++;
        rbs[i] = ctx->sh->next_rb++;
        ctx->sh->renderbuffers[rbs[i]];
    }
}

GL_APICALL void GL_APIENTRY glDeleteRenderbuffers(GLsizei n, const GLuint *rbs)
{
    GLES_CTX();
    for (GLsizei i = 0; i < n; i++) {
        const GLuint id = rbs[i];
        if (!id) continue;
        if (ctx->sh->renderbuffers.count(id)) texture_deleted(0xC000u + (id & 0x3FFF));
        ctx->sh->renderbuffers.erase(id);
        if (ctx->renderbuffer == id) ctx->renderbuffer = 0;
        for (auto &kv : ctx->fbos) {
            if (kv.second.color_rb == id) kv.second.color_rb = 0;
            if (kv.second.depth_rb == id) kv.second.depth_rb = 0;
            if (kv.second.stencil_rb == id) kv.second.stencil_rb = 0;
        }
    }
}

GL_APICALL GLboolean GL_APIENTRY glIsRenderbuffer(GLuint rb)
{
    GLES_CTX(GL_FALSE);
    return rb && ctx->sh->renderbuffers.count(rb) ? GL_TRUE : GL_FALSE;
}

GL_APICALL void GL_APIENTRY glBindRenderbuffer(GLenum target, GLuint rb)
{
    GLES_CTX();
    if (target != GL_RENDERBUFFER) return set_error(GL_INVALID_ENUM);
    if (rb) ctx->sh->renderbuffers[rb];
    ctx->renderbuffer = rb;
}

GL_APICALL void GL_APIENTRY glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                                                             GLsizei width, GLsizei height)
{
    GLES_CTX();
    if (target != GL_RENDERBUFFER) return set_error(GL_INVALID_ENUM);
    if (!ctx->renderbuffer) return set_error(GL_INVALID_OPERATION);
    if (width < 0 || height < 0) return set_error(GL_INVALID_VALUE);
    Renderbuffer &r = ctx->sh->renderbuffers[ctx->renderbuffer];
    r.format = internalformat;
    r.width = width;
    r.height = height;
    r.samples = samples;
}

GL_APICALL void GL_APIENTRY glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
{
    glRenderbufferStorageMultisample(target, 0, internalformat, width, height);
}

GL_APICALL void GL_APIENTRY glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params)
{
    GLES_CTX();
    if (target != GL_RENDERBUFFER || !ctx->renderbuffer) return set_error(GL_INVALID_OPERATION);
    const Renderbuffer &r = ctx->sh->renderbuffers[ctx->renderbuffer];
    const bool depth = r.format == GL_DEPTH_COMPONENT16 || r.format == GL_DEPTH_COMPONENT24 ||
                       r.format == GL_DEPTH24_STENCIL8 || r.format == GL_DEPTH_COMPONENT32F;
    switch (pname) {
    case GL_RENDERBUFFER_WIDTH: *params = r.width; break;
    case GL_RENDERBUFFER_HEIGHT: *params = r.height; break;
    case GL_RENDERBUFFER_INTERNAL_FORMAT: *params = static_cast<GLint>(r.format); break;
    case GL_RENDERBUFFER_SAMPLES: *params = r.samples; break;
    case GL_RENDERBUFFER_RED_SIZE: case GL_RENDERBUFFER_BLUE_SIZE: *params = depth ? 0 : 5; break;
    case GL_RENDERBUFFER_GREEN_SIZE: *params = depth ? 0 : 6; break;
    case GL_RENDERBUFFER_ALPHA_SIZE: *params = 0; break;
    case GL_RENDERBUFFER_DEPTH_SIZE: *params = depth ? 16 : 0; break;
    case GL_RENDERBUFFER_STENCIL_SIZE: *params = 0; break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

GL_APICALL void GL_APIENTRY glGenFramebuffers(GLsizei n, GLuint *fbs)
{
    GLES_CTX();
    if (n < 0) return set_error(GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; i++) {
        while (ctx->fbos.count(ctx->next_fbo)) ctx->next_fbo++;
        fbs[i] = ctx->next_fbo++;
        ctx->fbos[fbs[i]];
    }
}

GL_APICALL void GL_APIENTRY glDeleteFramebuffers(GLsizei n, const GLuint *fbs)
{
    GLES_CTX();
    for (GLsizei i = 0; i < n; i++) {
        if (!fbs[i]) continue;
        ctx->fbos.erase(fbs[i]);
        if (ctx->draw_fbo == fbs[i]) ctx->draw_fbo = 0;
        if (ctx->read_fbo == fbs[i]) ctx->read_fbo = 0;
    }
}

GL_APICALL GLboolean GL_APIENTRY glIsFramebuffer(GLuint fb)
{
    GLES_CTX(GL_FALSE);
    return fb && ctx->fbos.count(fb) ? GL_TRUE : GL_FALSE;
}

GL_APICALL void GL_APIENTRY glBindFramebuffer(GLenum target, GLuint fb)
{
    GLES_CTX();
    if (fb) ctx->fbos[fb];
    switch (target) {
    case GL_FRAMEBUFFER: ctx->draw_fbo = ctx->read_fbo = fb; break;
    case GL_DRAW_FRAMEBUFFER: ctx->draw_fbo = fb; break;
    case GL_READ_FRAMEBUFFER: ctx->read_fbo = fb; break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

static Framebuffer *target_fbo(Context *c, GLenum target)
{
    GLuint id;
    switch (target) {
    case GL_FRAMEBUFFER: case GL_DRAW_FRAMEBUFFER: id = c->draw_fbo; break;
    case GL_READ_FRAMEBUFFER: id = c->read_fbo; break;
    default: set_error(GL_INVALID_ENUM); return nullptr;
    }
    if (!id) {
        set_error(GL_INVALID_OPERATION);
        return nullptr;
    }
    return &c->fbos[id];
}

GL_APICALL void GL_APIENTRY glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                                                   GLint level)
{
    GLES_CTX();
    Framebuffer *f = target_fbo(ctx, target);
    if (!f) return;
    if (texture && textarget != GL_TEXTURE_2D) {
        warn_once("framebuffer attachments other than GL_TEXTURE_2D");
        return set_error(GL_INVALID_OPERATION);
    }
    if (level != 0) warn_once("framebuffer attachment level != 0");
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
        f->color_tex = texture;
        if (texture) f->color_rb = 0;
        break;
    case GL_DEPTH_ATTACHMENT: f->depth_tex = texture; break;
    case GL_STENCIL_ATTACHMENT: break;
    case GL_DEPTH_STENCIL_ATTACHMENT: f->depth_tex = texture; break;
    default:
        warn_once("multiple color attachments");
        set_error(GL_INVALID_ENUM);
        break;
    }
}

GL_APICALL void GL_APIENTRY glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                                      GLuint renderbuffer)
{
    GLES_CTX();
    Framebuffer *f = target_fbo(ctx, target);
    if (!f) return;
    if (renderbuffertarget != GL_RENDERBUFFER) return set_error(GL_INVALID_ENUM);
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
        f->color_rb = renderbuffer;
        if (renderbuffer) f->color_tex = 0;
        break;
    case GL_DEPTH_ATTACHMENT: f->depth_rb = renderbuffer; break;
    case GL_STENCIL_ATTACHMENT: f->stencil_rb = renderbuffer; break;
    case GL_DEPTH_STENCIL_ATTACHMENT: f->depth_rb = f->stencil_rb = renderbuffer; break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

GL_APICALL GLenum GL_APIENTRY glCheckFramebufferStatus(GLenum target)
{
    GLES_CTX(0);
    GLuint id = (target == GL_READ_FRAMEBUFFER) ? ctx->read_fbo : ctx->draw_fbo;
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) {
        set_error(GL_INVALID_ENUM);
        return 0;
    }
    if (!id) return GL_FRAMEBUFFER_COMPLETE;
    const Framebuffer &f = ctx->fbos[id];
    int w = 0, h = 0;
    if (f.color_tex) {
        auto it = ctx->sh->textures.find(f.color_tex);
        if (it == ctx->sh->textures.end()) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        w = it->second.width;
        h = it->second.height;
    } else if (f.color_rb) {
        auto it = ctx->sh->renderbuffers.find(f.color_rb);
        if (it == ctx->sh->renderbuffers.end()) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        w = it->second.width;
        h = it->second.height;
    } else {
        return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    }
    if (w <= 0 || h <= 0) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    if (static_cast<uint32_t>(w) > device().max_tex_dim || static_cast<uint32_t>(h) > device().max_tex_dim) {
        return GL_FRAMEBUFFER_UNSUPPORTED;
    }
    return GL_FRAMEBUFFER_COMPLETE;
}

GL_APICALL void GL_APIENTRY glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname,
                                                                  GLint *params)
{
    GLES_CTX();
    GLuint id = (target == GL_READ_FRAMEBUFFER) ? ctx->read_fbo : ctx->draw_fbo;
    if (!id) {
        /* 默认帧缓冲 */
        switch (pname) {
        case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE: *params = GL_FRAMEBUFFER_DEFAULT; return;
        case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE: *params = 5; return;
        case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE: *params = 6; return;
        case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE: *params = 16; return;
        case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE: *params = 0; return;
        case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE: *params = GL_UNSIGNED_NORMALIZED; return;
        case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING: *params = 0x2601 /* GL_LINEAR */; return;
        default: set_error(GL_INVALID_ENUM); return;
        }
    }
    const Framebuffer &f = ctx->fbos[id];
    GLuint tex = 0, rb = 0;
    if (attachment == GL_COLOR_ATTACHMENT0) {
        tex = f.color_tex;
        rb = f.color_rb;
    } else if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        tex = f.depth_tex;
        rb = f.depth_rb;
    } else if (attachment == GL_STENCIL_ATTACHMENT) {
        rb = f.stencil_rb;
    }
    switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE: *params = tex ? GL_TEXTURE : rb ? GL_RENDERBUFFER : GL_NONE; break;
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME: *params = static_cast<GLint>(tex ? tex : rb); break;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL: case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE: *params = 0; break;
    default: *params = 0; break;
    }
}

GL_APICALL void GL_APIENTRY glDrawBuffers(GLsizei n, const GLenum *bufs)
{
    if (n > 1 || (n == 1 && bufs[0] != GL_BACK && bufs[0] != GL_COLOR_ATTACHMENT0 && bufs[0] != GL_NONE)) {
        warn_once("multiple draw buffers");
    }
}

GL_APICALL void GL_APIENTRY glReadBuffer(GLenum src)
{
    GLES_CTX();
    ctx->read_buffer = src;
}

GL_APICALL void GL_APIENTRY glInvalidateFramebuffer(GLenum, GLsizei, const GLenum *) {}
GL_APICALL void GL_APIENTRY glInvalidateSubFramebuffer(GLenum, GLsizei, const GLenum *, GLint, GLint, GLsizei, GLsizei) {}

GL_APICALL void GL_APIENTRY glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
{
    warn_once("glBlitFramebuffer");
    set_error(GL_INVALID_OPERATION);
}

/* ── 着色器 ── */

GL_APICALL GLuint GL_APIENTRY glCreateShader(GLenum type)
{
    GLES_CTX(0);
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        set_error(GL_INVALID_ENUM);
        return 0;
    }
    const GLuint id = ctx->sh->next_object++;
    auto s = std::make_unique<Shader>();
    s->type = type;
    ctx->sh->shaders[id] = std::move(s);
    return id;
}

GL_APICALL void GL_APIENTRY glDeleteShader(GLuint shader)
{
    GLES_CTX();
    if (!shader) return;
    Shader *s = get_shader(ctx, shader);
    if (!s) return;
    s->delete_pending = true;
    maybe_delete_shader(ctx, shader);
}

GL_APICALL GLboolean GL_APIENTRY glIsShader(GLuint shader)
{
    GLES_CTX(GL_FALSE);
    return ctx->sh->shaders.count(shader) ? GL_TRUE : GL_FALSE;
}

GL_APICALL void GL_APIENTRY glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
{
    GLES_CTX();
    Shader *s = get_shader(ctx, shader);
    if (!s) return;
    if (count < 0) return set_error(GL_INVALID_VALUE);
    s->source.clear();
    for (GLsizei i = 0; i < count; i++) {
        if (!string[i]) continue;
        if (length && length[i] >= 0) s->source.append(string[i], static_cast<size_t>(length[i]));
        else s->source.append(string[i]);
    }
}

GL_APICALL void GL_APIENTRY glCompileShader(GLuint shader)
{
    GLES_CTX();
    Shader *s = get_shader(ctx, shader);
    if (!s) return;
    auto r = std::make_shared<glsl::Shader>(
        glsl::compile(s->type == GL_VERTEX_SHADER ? glsl::Stage::Vertex : glsl::Stage::Fragment, s->source));
    s->compile_attempted = true;
    s->compiled = r->ok;
    s->log = r->log;
    if (r->ok && r->stage == glsl::Stage::Fragment) {
        s->log += r->frag.program
                      ? "info: compiled to " + std::to_string(r->frag.code.size()) + " VM instructions\n"
                      : "info: matched built-in shade mode " + std::to_string(r->frag.shade_mode) + "\n";
    }
    if (r->ok && r->stage == glsl::Stage::Vertex) {
        s->log += r->vert.tnl ? "info: vertex transform runs on the device (T&L)\n"
                              : "info: vertex shader runs on the host CPU\n";
    }
    s->result = r;
}

GL_APICALL void GL_APIENTRY glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
    GLES_CTX();
    Shader *s = get_shader(ctx, shader);
    if (!s) return;
    switch (pname) {
    case GL_SHADER_TYPE: *params = static_cast<GLint>(s->type); break;
    case GL_DELETE_STATUS: *params = s->delete_pending; break;
    case GL_COMPILE_STATUS: *params = s->compiled; break;
    case GL_INFO_LOG_LENGTH: *params = s->log.empty() ? 0 : static_cast<GLint>(s->log.size() + 1); break;
    case GL_SHADER_SOURCE_LENGTH: *params = s->source.empty() ? 0 : static_cast<GLint>(s->source.size() + 1); break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

GL_APICALL void GL_APIENTRY glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
    GLES_CTX();
    Shader *s = get_shader(ctx, shader);
    if (!s) return;
    copy_str(s->log, bufSize, length, infoLog);
}

GL_APICALL void GL_APIENTRY glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source)
{
    GLES_CTX();
    Shader *s = get_shader(ctx, shader);
    if (!s) return;
    copy_str(s->source, bufSize, length, source);
}

GL_APICALL void GL_APIENTRY glGetShaderPrecisionFormat(GLenum, GLenum precisiontype, GLint *range, GLint *precision)
{
    if (precisiontype >= GL_LOW_INT && precisiontype <= GL_HIGH_INT) {
        /* 整数在 VM 里是 float：精确到 2^24 */
        range[0] = range[1] = 24;
        *precision = 0;
    } else {
        range[0] = range[1] = 127;
        *precision = 23;
    }
}

GL_APICALL void GL_APIENTRY glReleaseShaderCompiler(void) {}

GL_APICALL void GL_APIENTRY glShaderBinary(GLsizei, const GLuint *, GLenum, const void *, GLsizei)
{
    set_error(GL_INVALID_ENUM);
}

/* ── 程序 ── */

GL_APICALL GLuint GL_APIENTRY glCreateProgram(void)
{
    GLES_CTX(0);
    const GLuint id = ctx->sh->next_object++;
    ctx->sh->programs[id] = std::make_unique<Program>();
    return id;
}

GL_APICALL void GL_APIENTRY glDeleteProgram(GLuint program)
{
    GLES_CTX();
    if (!program) return;
    Program *p = get_program(ctx, program);
    if (!p) return;
    if (ctx->program == program) {
        p->delete_pending = true;
        return;
    }
    const GLuint vs = p->vs, fs = p->fs;
    ctx->sh->programs.erase(program);
    for (GLuint s : {vs, fs}) {
        auto it = ctx->sh->shaders.find(s);
        if (it != ctx->sh->shaders.end()) {
            it->second->attach_count--;
            maybe_delete_shader(ctx, s);
        }
    }
}

GL_APICALL GLboolean GL_APIENTRY glIsProgram(GLuint program)
{
    GLES_CTX(GL_FALSE);
    return ctx->sh->programs.count(program) ? GL_TRUE : GL_FALSE;
}

GL_APICALL void GL_APIENTRY glAttachShader(GLuint program, GLuint shader)
{
    GLES_CTX();
    Program *p = get_program(ctx, program);
    Shader *s = get_shader(ctx, shader);
    if (!p || !s) return;
    GLuint &slot = s->type == GL_VERTEX_SHADER ? p->vs : p->fs;
    if (slot) return set_error(GL_INVALID_OPERATION);
    slot = shader;
    s->attach_count++;
}

GL_APICALL void GL_APIENTRY glDetachShader(GLuint program, GLuint shader)
{
    GLES_CTX();
    Program *p = get_program(ctx, program);
    Shader *s = get_shader(ctx, shader);
    if (!p || !s) return;
    GLuint &slot = s->type == GL_VERTEX_SHADER ? p->vs : p->fs;
    if (slot != shader) return set_error(GL_INVALID_OPERATION);
    slot = 0;
    s->attach_count--;
    maybe_delete_shader(ctx, shader);
}

GL_APICALL void GL_APIENTRY glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders)
{
    GLES_CTX();
    Program *p = get_program(ctx, program);
    if (!p) return;
    GLsizei n = 0;
    for (GLuint s : {p->vs, p->fs}) {
        if (s && n < maxCount) shaders[n++] = s;
    }
    if (count) *count = n;
}

GL_APICALL void GL_APIENTRY glBindAttribLocation(GLuint program, GLuint index, const GLchar *name)
{
    GLES_CTX();
    Program *p = get_program(ctx, program);
    if (!p) return;
    if (index >= kMaxAttribs) return set_error(GL_INVALID_VALUE);
    if (std::strncmp(name, "gl_", 3) == 0) return set_error(GL_INVALID_OPERATION);
    p->bound_attribs[name] = index;
}

GL_APICALL void GL_APIENTRY glLinkProgram(GLuint program)
{
    GLES_CTX();
    Program *p = get_program(ctx, program);
    if (!p) return;
    p->linked = false;
    p->log.clear();
    Shader *vs = p->vs ? get_shader(ctx, p->vs, false) : nullptr;
    Shader *fs = p->fs ? get_shader(ctx, p->fs, false) : nullptr;
    if (!vs || !fs) {
        p->log = "program needs both a vertex and a fragment shader\n";
        return;
    }
    if (!vs->compiled || !fs->compiled || !vs->result || !fs->result) {
        p->log = "attached shaders are not compiled successfully\n";
        return;
    }
    glsl::Program lk = glsl::link(*vs->result, *fs->result);
    if (!lk.ok) {
        p->log = lk.log;
        return;
    }
    p->vsc = vs->result;
    p->fsc = fs->result;
    p->link = lk;
    p->storage.assign(static_cast<size_t>(lk.storage_slots), {0, 0, 0, 0});

    /* uniform 位置 */
    p->locations.clear();
    p->loc_by_name.clear();
    for (size_t i = 0; i < static_cast<size_t>(lk.visible_uniforms); i++) {
        const auto &u = lk.uniforms[i];
        for (int e = 0; e < u.array_size; e++) {
            const GLint loc = static_cast<GLint>(p->locations.size());
            p->locations.push_back({static_cast<int>(i), e});
            if (u.array_size > 1) {
                p->loc_by_name[u.name + "[" + std::to_string(e) + "]"] = loc;
                if (e == 0) p->loc_by_name[u.name] = loc;
            } else {
                p->loc_by_name[u.name] = loc;
            }
        }
    }

    /* 属性位置：layout(location) > glBindAttribLocation > 自动分配 */
    const auto &ins = p->vsc->inputs;
    p->attrib_loc.assign(ins.size(), -1);
    std::set<int> taken;
    for (size_t i = 0; i < ins.size(); i++) {
        int loc = ins[i].location;
        if (loc < 0) {
            auto it = p->bound_attribs.find(ins[i].name);
            if (it != p->bound_attribs.end()) loc = static_cast<int>(it->second);
        }
        if (loc >= kMaxAttribs) {
            p->log = "attribute location out of range for '" + ins[i].name + "'\n";
            return;
        }
        if (loc >= 0) {
            p->attrib_loc[i] = loc;
            taken.insert(loc);
        }
    }
    for (size_t i = 0; i < ins.size(); i++) {
        if (p->attrib_loc[i] >= 0) continue;
        int loc = 0;
        while (taken.count(loc)) loc++;
        if (loc >= kMaxAttribs) {
            p->log = "too many vertex attributes\n";
            return;
        }
        p->attrib_loc[i] = loc;
        taken.insert(loc);
    }
    p->serial = g_program_serial++;
    p->storage_version++;
    p->linked = true;
    p->log = "link ok\n";
}

GL_APICALL void GL_APIENTRY glUseProgram(GLuint program)
{
    GLES_CTX();
    if (program) {
        Program *p = get_program(ctx, program);
        if (!p) return;
        if (!p->linked) return set_error(GL_INVALID_OPERATION);
    }
    const GLuint old = ctx->program;
    ctx->program = program;
    if (old && old != program) {
        auto it = ctx->sh->programs.find(old);
        if (it != ctx->sh->programs.end() && it->second->delete_pending) glDeleteProgram(old);
    }
}

GL_APICALL void GL_APIENTRY glValidateProgram(GLuint program)
{
    GLES_CTX();
    Program *p = get_program(ctx, program);
    if (p) p->validated = p->linked;
}

GL_APICALL void GL_APIENTRY glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
    GLES_CTX();
    Program *p = get_program(ctx, program);
    if (!p) return;
    switch (pname) {
    case GL_DELETE_STATUS: *params = p->delete_pending; break;
    case GL_LINK_STATUS: *params = p->linked; break;
    case GL_VALIDATE_STATUS: *params = p->validated; break;
    case GL_INFO_LOG_LENGTH: *params = p->log.empty() ? 0 : static_cast<GLint>(p->log.size() + 1); break;
    case GL_ATTACHED_SHADERS: *params = (p->vs ? 1 : 0) + (p->fs ? 1 : 0); break;
    case GL_ACTIVE_UNIFORMS: *params = p->linked ? p->link.visible_uniforms : 0; break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: {
        GLint m = 0;
        for (int i = 0; i < p->link.visible_uniforms; i++)
            m = std::max<GLint>(m, static_cast<GLint>(p->link.uniforms[static_cast<size_t>(i)].name.size() + 4));
        *params = p->linked ? m : 0;
        break;
    }
    case GL_ACTIVE_ATTRIBUTES: *params = p->linked ? static_cast<GLint>(p->vsc->inputs.size()) : 0; break;
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: {
        GLint m = 0;
        if (p->linked)
            for (const auto &a : p->vsc->inputs) m = std::max<GLint>(m, static_cast<GLint>(a.name.size() + 1));
        *params = m;
        break;
    }
    case GL_ACTIVE_UNIFORM_BLOCKS: case GL_TRANSFORM_FEEDBACK_VARYINGS: case GL_PROGRAM_BINARY_LENGTH:
    case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH: *params = 0; break;
    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE: *params = GL_INTERLEAVED_ATTRIBS; break;
    case GL_PROGRAM_BINARY_RETRIEVABLE_HINT: *params = 0; break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}

GL_APICALL void GL_APIENTRY glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
    GLES_CTX();
    Program *p = get_program(ctx, program);
    if (!p) return;
    copy_str(p->log, bufSize, length, infoLog);
}

GL_APICALL GLint GL_APIENTRY glGetAttribLocation(GLuint program, const GLchar *name)
{
    GLES_CTX(-1);
    Program *p = get_program(ctx, program);
    if (!p) return -1;
    if (!p->linked) {
        set_error(GL_INVALID_OPERATION);
        return -1;
    }
    for (size_t i = 0; i < p->vsc->inputs.size(); i++) {
        if (p->vsc->inputs[i].name == name) return p->attrib_loc[i];
    }
    return -1;
}

GL_APICALL void GL_APIENTRY glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length,
                                              GLint *size, GLenum *type, GLchar *name)
{
    GLES_CTX();
    Program *p = get_program(ctx, program);
    if (!p) return;
    if (!p->linked || index >= p->vsc->inputs.size()) return set_error(GL_INVALID_VALUE);
    const auto &a = p->vsc->inputs[index];
    static const GLenum ft[4] = {GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4};
    static const GLenum it[4] = {GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4};
    *size = 1;
    *type = (a.is_int ? it : ft)[a.components - 1];
    copy_str(a.name, bufSize, length, name);
}

GL_APICALL void GL_APIENTRY glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length,
                                               GLint *size, GLenum *type, GLchar *name)
{
    GLES_CTX();
    Program *p = get_program(ctx, program);
    if (!p) return;
    if (!p->linked || index >= static_cast<GLuint>(p->link.visible_uniforms)) return set_error(GL_INVALID_VALUE);
    const auto &u = p->link.uniforms[index];
    *size = u.array_size;
    *type = u.gl_type;
    copy_str(u.array_size > 1 ? u.name + "[0]" : u.name, bufSize, length, name);
}

GL_APICALL GLint GL_APIENTRY glGetUniformLocation(GLuint program, const GLchar *name)
{
    GLES_CTX(-1);
    Program *p = get_program(ctx, program);
    if (!p) return -1;
    if (!p->linked) {
        set_error(GL_INVALID_OPERATION);
        return -1;
    }
    auto it = p->loc_by_name.find(name);
    if (it != p->loc_by_name.end()) return it->second;
    /* "a[0]" 对非数组也合法 */
    std::string n = name;
    if (n.size() > 3 && n.compare(n.size() - 3, 3, "[0]") == 0) {
        it = p->loc_by_name.find(n.substr(0, n.size() - 3));
        if (it != p->loc_by_name.end()) return it->second;
    }
    return -1;
}

GL_APICALL GLuint GL_APIENTRY glGetUniformBlockIndex(GLuint, const GLchar *)
{
    return GL_INVALID_INDEX;
}

GL_APICALL GLint GL_APIENTRY glGetFragDataLocation(GLuint program, const GLchar *name)
{
    GLES_CTX(-1);
    Program *p = get_program(ctx, program);
    if (!p || !p->linked) return -1;
    for (size_t i = 0; i < p->fsc->outputs.size(); i++) {
        if (p->fsc->outputs[i].name == name) return p->fsc->outputs[i].location >= 0 ? p->fsc->outputs[i].location : 0;
    }
    return -1;
}

static void get_uniform(GLuint program, GLint location, float *out, int *n)
{
    GLES_CTX();
    *n = 0;
    Program *p = get_program(ctx, program);
    if (!p || !p->linked || location < 0 || location >= static_cast<GLint>(p->locations.size())) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    const UniformLoc &ul = p->locations[static_cast<size_t>(location)];
    const auto &u = p->link.uniforms[static_cast<size_t>(ul.uniform)];
    const int cols = u.is_sampler ? 1 : u.columns;
    const int rows = u.is_sampler ? 1 : u.components;
    for (int c = 0; c < cols; c++)
        for (int r = 0; r < rows; r++) out[(*n)++] = p->storage[static_cast<size_t>(u.storage + ul.element * cols + c)][static_cast<size_t>(r)];
}

GL_APICALL void GL_APIENTRY glGetUniformfv(GLuint program, GLint location, GLfloat *params)
{
    float v[16];
    int n;
    get_uniform(program, location, v, &n);
    for (int i = 0; i < n; i++) params[i] = v[i];
}

GL_APICALL void GL_APIENTRY glGetUniformiv(GLuint program, GLint location, GLint *params)
{
    float v[16];
    int n;
    get_uniform(program, location, v, &n);
    for (int i = 0; i < n; i++) params[i] = static_cast<GLint>(v[i]);
}

GL_APICALL void GL_APIENTRY glGetUniformuiv(GLuint program, GLint location, GLuint *params)
{
    float v[16];
    int n;
    get_uniform(program, location, v, &n);
    for (int i = 0; i < n; i++) params[i] = static_cast<GLuint>(v[i]);
}

/* uniform 写入 */
GL_APICALL void GL_APIENTRY glUniform1f(GLint l, GLfloat a) { float v[] = {a}; set_uniform(l, 1, 0, 1, v); }
GL_APICALL void GL_APIENTRY glUniform2f(GLint l, GLfloat a, GLfloat b) { float v[] = {a, b}; set_uniform(l, 2, 0, 1, v); }
GL_APICALL void GL_APIENTRY glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { float v[] = {a, b, c}; set_uniform(l, 3, 0, 1, v); }
GL_APICALL void GL_APIENTRY glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { float v[] = {a, b, c, d}; set_uniform(l, 4, 0, 1, v); }
GL_APICALL void GL_APIENTRY glUniform1i(GLint l, GLint a) { GLint v[] = {a}; set_uniform(l, 1, 1, 1, v); }
GL_APICALL void GL_APIENTRY glUniform2i(GLint l, GLint a, GLint b) { GLint v[] = {a, b}; set_uniform(l, 2, 1, 1, v); }
GL_APICALL void GL_APIENTRY glUniform3i(GLint l, GLint a, GLint b, GLint c) { GLint v[] = {a, b, c}; set_uniform(l, 3, 1, 1, v); }
GL_APICALL void GL_APIENTRY glUniform4i(GLint l, GLint a, GLint b, GLint c, GLint d) { GLint v[] = {a, b, c, d}; set_uniform(l, 4, 1, 1, v); }
GL_APICALL void GL_APIENTRY glUniform1ui(GLint l, GLuint a) { GLuint v[] = {a}; set_uniform(l, 1, 2, 1, v); }
GL_APICALL void GL_APIENTRY glUniform2ui(GLint l, GLuint a, GLuint b) { GLuint v[] = {a, b}; set_uniform(l, 2, 2, 1, v); }
GL_APICALL void GL_APIENTRY glUniform3ui(GLint l, GLuint a, GLuint b, GLuint c) { GLuint v[] = {a, b, c}; set_uniform(l, 3, 2, 1, v); }
GL_APICALL void GL_APIENTRY glUniform4ui(GLint l, GLuint a, GLuint b, GLuint c, GLuint d) { GLuint v[] = {a, b, c, d}; set_uniform(l, 4, 2, 1, v); }
GL_APICALL void GL_APIENTRY glUniform1fv(GLint l, GLsizei n, const GLfloat *v) { set_uniform(l, 1, 0, n, v); }
GL_APICALL void GL_APIENTRY glUniform2fv(GLint l, GLsizei n, const GLfloat *v) { set_uniform(l, 2, 0, n, v); }
GL_APICALL void GL_APIENTRY glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { set_uniform(l, 3, 0, n, v); }
GL_APICALL void GL_APIENTRY glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { set_uniform(l, 4, 0, n, v); }
GL_APICALL void GL_APIENTRY glUniform1iv(GLint l, GLsizei n, const GLint *v) { set_uniform(l, 1, 1, n, v); }
GL_APICALL void GL_APIENTRY glUniform2iv(GLint l, GLsizei n, const GLint *v) { set_uniform(l, 2, 1, n, v); }
GL_APICALL void GL_APIENTRY glUniform3iv(GLint l, GLsizei n, const GLint *v) { set_uniform(l, 3, 1, n, v); }
GL_APICALL void GL_APIENTRY glUniform4iv(GLint l, GLsizei n, const GLint *v) { set_uniform(l, 4, 1, n, v); }
GL_APICALL void GL_APIENTRY glUniform1uiv(GLint l, GLsizei n, const GLuint *v) { set_uniform(l, 1, 2, n, v); }
GL_APICALL void GL_APIENTRY glUniform2uiv(GLint l, GLsizei n, const GLuint *v) { set_uniform(l, 2, 2, n, v); }
GL_APICALL void GL_APIENTRY glUniform3uiv(GLint l, GLsizei n, const GLuint *v) { set_uniform(l, 3, 2, n, v); }
GL_APICALL void GL_APIENTRY glUniform4uiv(GLint l, GLsizei n, const GLuint *v) { set_uniform(l, 4, 2, n, v); }
GL_APICALL void GL_APIENTRY glUniformMatrix2fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { set_uniform(l, 0, 0, n, v, 2, 2, t != 0); }
GL_APICALL void GL_APIENTRY glUniformMatrix3fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { set_uniform(l, 0, 0, n, v, 3, 3, t != 0); }
GL_APICALL void GL_APIENTRY glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { set_uniform(l, 0, 0, n, v, 4, 4, t != 0); }
GL_APICALL void GL_APIENTRY glUniformMatrix2x3fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { set_uniform(l, 0, 0, n, v, 2, 3, t != 0); }
GL_APICALL void GL_APIENTRY glUniformMatrix3x2fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { set_uniform(l, 0, 0, n, v, 3, 2, t != 0); }
GL_APICALL void GL_APIENTRY glUniformMatrix2x4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { set_uniform(l, 0, 0, n, v, 2, 4, t != 0); }
GL_APICALL void GL_APIENTRY glUniformMatrix4x2fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { set_uniform(l, 0, 0, n, v, 4, 2, t != 0); }
GL_APICALL void GL_APIENTRY glUniformMatrix3x4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { set_uniform(l, 0, 0, n, v, 3, 4, t != 0); }
GL_APICALL void GL_APIENTRY glUniformMatrix4x3fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { set_uniform(l, 0, 0, n, v, 4, 3, t != 0); }

/* ── 固定功能状态 ── */

GL_APICALL void GL_APIENTRY glEnable(GLenum cap) { enable_cap(cap, true); }
GL_APICALL void GL_APIENTRY glDisable(GLenum cap) { enable_cap(cap, false); }

GL_APICALL GLboolean GL_APIENTRY glIsEnabled(GLenum cap)
{
    GLES_CTX(GL_FALSE);
    switch (cap) {
    case GL_BLEND: return ctx->blend;
    case GL_DEPTH_TEST: return ctx->depth_test;
    case GL_CULL_FACE: return ctx->cull;
    case GL_SCISSOR_TEST: return ctx->scissor_test;
    case GL_POLYGON_OFFSET_FILL: return ctx->polygon_offset;
    case GL_DITHER: return ctx->dither;
    case GL_STENCIL_TEST: return ctx->stencil_test;
    case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE: case GL_RASTERIZER_DISCARD:
    case GL_PRIMITIVE_RESTART_FIXED_INDEX: return GL_FALSE;
    default: set_error(GL_INVALID_ENUM); return GL_FALSE;
    }
}

GL_APICALL void GL_APIENTRY glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da)
{
    GLES_CTX();
    ctx->blend_src_rgb = sr;
    ctx->blend_dst_rgb = dr;
    ctx->blend_src_a = sa;
    ctx->blend_dst_a = da;
}

GL_APICALL void GL_APIENTRY glBlendFunc(GLenum s, GLenum d) { glBlendFuncSeparate(s, d, s, d); }

GL_APICALL void GL_APIENTRY glBlendEquationSeparate(GLenum rgb, GLenum a)
{
    GLES_CTX();
    ctx->blend_eq_rgb = rgb;
    ctx->blend_eq_a = a;
}

GL_APICALL void GL_APIENTRY glBlendEquation(GLenum mode) { glBlendEquationSeparate(mode, mode); }

GL_APICALL void GL_APIENTRY glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    GLES_CTX();
    ctx->blend_color[0] = r;
    ctx->blend_color[1] = g;
    ctx->blend_color[2] = b;
    ctx->blend_color[3] = a;
}

GL_APICALL void GL_APIENTRY glDepthFunc(GLenum func)
{
    GLES_CTX();
    if (func < GL_NEVER || func > GL_ALWAYS) return set_error(GL_INVALID_ENUM);
    ctx->depth_func = func;
}

GL_APICALL void GL_APIENTRY glDepthMask(GLboolean flag)
{
    GLES_CTX();
    ctx->depth_mask = flag != 0;
}

GL_APICALL void GL_APIENTRY glDepthRangef(GLfloat n, GLfloat f)
{
    GLES_CTX();
    ctx->depth_near = std::min(std::max(n, 0.0f), 1.0f);
    ctx->depth_far = std::min(std::max(f, 0.0f), 1.0f);
}

GL_APICALL void GL_APIENTRY glCullFace(GLenum mode)
{
    GLES_CTX();
    if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) return set_error(GL_INVALID_ENUM);
    ctx->cull_face = mode;
}

GL_APICALL void GL_APIENTRY glFrontFace(GLenum mode)
{
    GLES_CTX();
    if (mode != GL_CW && mode != GL_CCW) return set_error(GL_INVALID_ENUM);
    ctx->front_face = mode;
}

GL_APICALL void GL_APIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    GLES_CTX();
    ctx->color_mask[0] = r != 0;
    ctx->color_mask[1] = g != 0;
    ctx->color_mask[2] = b != 0;
    ctx->color_mask[3] = a != 0;
    if ((r || g || b) && !(r && g && b)) warn_once("partial color write masks (treated as all-or-nothing)");
}

GL_APICALL void GL_APIENTRY glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    GLES_CTX();
    ctx->clear_color[0] = r;
    ctx->clear_color[1] = g;
    ctx->clear_color[2] = b;
    ctx->clear_color[3] = a;
}

GL_APICALL void GL_APIENTRY glClearDepthf(GLfloat d)
{
    GLES_CTX();
    ctx->clear_depth = std::min(std::max(d, 0.0f), 1.0f);
}

GL_APICALL void GL_APIENTRY glClearStencil(GLint s)
{
    GLES_CTX();
    ctx->clear_stencil = s;
}

GL_APICALL void GL_APIENTRY glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    GLES_CTX();
    if (w < 0 || h < 0) return set_error(GL_INVALID_VALUE);
    ctx->viewport[0] = x;
    ctx->viewport[1] = y;
    ctx->viewport[2] = w;
    ctx->viewport[3] = h;
    ctx->viewport_set = true;
}

GL_APICALL void GL_APIENTRY glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    GLES_CTX();
    if (w < 0 || h < 0) return set_error(GL_INVALID_VALUE);
    ctx->scissor[0] = x;
    ctx->scissor[1] = y;
    ctx->scissor[2] = w;
    ctx->scissor[3] = h;
}

GL_APICALL void GL_APIENTRY glLineWidth(GLfloat width)
{
    GLES_CTX();
    if (width <= 0) return set_error(GL_INVALID_VALUE);
    ctx->line_width = width;
}

GL_APICALL void GL_APIENTRY glPolygonOffset(GLfloat factor, GLfloat units)
{
    GLES_CTX();
    ctx->poly_factor = factor;
    ctx->poly_units = units;
}

GL_APICALL void GL_APIENTRY glHint(GLenum target, GLenum mode)
{
    GLES_CTX();
    if (target == GL_GENERATE_MIPMAP_HINT) ctx->hint_mipmap = mode;
}

GL_APICALL void GL_APIENTRY glSampleCoverage(GLfloat, GLboolean) {}

GL_APICALL void GL_APIENTRY glStencilFuncSeparate(GLenum, GLenum func, GLint ref, GLuint mask)
{
    GLES_CTX();
    ctx->stencil_func = func;
    ctx->stencil_ref = ref;
    ctx->stencil_value_mask = mask;
}
GL_APICALL void GL_APIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask) { glStencilFuncSeparate(GL_FRONT_AND_BACK, func, ref, mask); }
GL_APICALL void GL_APIENTRY glStencilMaskSeparate(GLenum, GLuint mask)
{
    GLES_CTX();
    ctx->stencil_writemask = mask;
}
GL_APICALL void GL_APIENTRY glStencilMask(GLuint mask) { glStencilMaskSeparate(GL_FRONT_AND_BACK, mask); }
GL_APICALL void GL_APIENTRY glStencilOpSeparate(GLenum, GLenum f, GLenum zf, GLenum zp)
{
    GLES_CTX();
    ctx->stencil_fail = f;
    ctx->stencil_zfail = zf;
    ctx->stencil_zpass = zp;
}
GL_APICALL void GL_APIENTRY glStencilOp(GLenum f, GLenum zf, GLenum zp) { glStencilOpSeparate(GL_FRONT_AND_BACK, f, zf, zp); }

/* ── 查询 ── */

GL_APICALL void GL_APIENTRY glGetIntegerv(GLenum pname, GLint *data)
{
    GLES_CTX();
    double v[16];
    int n;
    if (!get_state(pname, v, n)) return set_error(GL_INVALID_ENUM);
    const bool color = pname == GL_COLOR_CLEAR_VALUE || pname == GL_BLEND_COLOR || pname == GL_DEPTH_RANGE ||
                       pname == GL_DEPTH_CLEAR_VALUE;
    for (int i = 0; i < n; i++) {
        data[i] = color ? static_cast<GLint>(v[i] * 2147483647.0) : static_cast<GLint>(static_cast<int64_t>(v[i]));
    }
}

GL_APICALL void GL_APIENTRY glGetInteger64v(GLenum pname, GLint64 *data)
{
    GLES_CTX();
    double v[16];
    int n;
    if (!get_state(pname, v, n)) return set_error(GL_INVALID_ENUM);
    for (int i = 0; i < n; i++) data[i] = static_cast<GLint64>(v[i]);
}

GL_APICALL void GL_APIENTRY glGetIntegeri_v(GLenum, GLuint, GLint *data)
{
    *data = 0;
}

GL_APICALL void GL_APIENTRY glGetFloatv(GLenum pname, GLfloat *data)
{
    GLES_CTX();
    double v[16];
    int n;
    if (!get_state(pname, v, n)) return set_error(GL_INVALID_ENUM);
    for (int i = 0; i < n; i++) data[i] = static_cast<GLfloat>(v[i]);
}

GL_APICALL void GL_APIENTRY glGetBooleanv(GLenum pname, GLboolean *data)
{
    GLES_CTX();
    double v[16];
    int n;
    if (!get_state(pname, v, n)) return set_error(GL_INVALID_ENUM);
    for (int i = 0; i < n; i++) data[i] = v[i] != 0 ? GL_TRUE : GL_FALSE;
}

GL_APICALL const GLubyte *GL_APIENTRY glGetString(GLenum name)
{
    GLES_CTX(nullptr);
    static std::string ext;
    if (ext.empty()) {
        for (const char *e : kExtensions) {
            ext += e;
            ext += ' ';
        }
    }
    switch (name) {
    case GL_VENDOR: return reinterpret_cast<const GLubyte *>("espgpu");
    case GL_RENDERER: return reinterpret_cast<const GLubyte *>("ESP32-S3 GPU (espgpu GLES 3.0 subset)");
    case GL_VERSION:
        return reinterpret_cast<const GLubyte *>(ctx->version >= 3 ? "OpenGL ES 3.0 espgpu" : "OpenGL ES 2.0 espgpu");
    case GL_SHADING_LANGUAGE_VERSION:
        return reinterpret_cast<const GLubyte *>(ctx->version >= 3 ? "OpenGL ES GLSL ES 3.00" : "OpenGL ES GLSL ES 1.00");
    case GL_EXTENSIONS: return reinterpret_cast<const GLubyte *>(ext.c_str());
    default: set_error(GL_INVALID_ENUM); return nullptr;
    }
}

GL_APICALL const GLubyte *GL_APIENTRY glGetStringi(GLenum name, GLuint index)
{
    if (name != GL_EXTENSIONS || index >= sizeof(kExtensions) / sizeof(kExtensions[0])) {
        set_error(GL_INVALID_VALUE);
        return nullptr;
    }
    return reinterpret_cast<const GLubyte *>(kExtensions[index]);
}

GL_APICALL void GL_APIENTRY glGetInternalformativ(GLenum, GLenum, GLenum pname, GLsizei count, GLint *params)
{
    if (count <= 0) return;
    params[0] = (pname == GL_NUM_SAMPLE_COUNTS) ? 0 : 0;
}

/* ── 同步 ── */

GL_APICALL GLsync GL_APIENTRY glFenceSync(GLenum condition, GLbitfield flags)
{
    GLES_CTX(nullptr);
    if (condition != GL_SYNC_GPU_COMMANDS_COMPLETE || flags != 0) {
        set_error(GL_INVALID_ENUM);
        return nullptr;
    }
    Device &d = device();
    const GLuint id = ctx->next_sync++;
    Sync s;
    s.fence = ++d.fence_seq;
    if (d.ok) espgpu_cmd_fence(d.dev, s.fence);
    ctx->syncs[id] = s;
    return reinterpret_cast<GLsync>(static_cast<uintptr_t>(id));
}

GL_APICALL GLboolean GL_APIENTRY glIsSync(GLsync sync)
{
    GLES_CTX(GL_FALSE);
    return ctx->syncs.count(static_cast<GLuint>(reinterpret_cast<uintptr_t>(sync))) ? GL_TRUE : GL_FALSE;
}

GL_APICALL void GL_APIENTRY glDeleteSync(GLsync sync)
{
    GLES_CTX();
    ctx->syncs.erase(static_cast<GLuint>(reinterpret_cast<uintptr_t>(sync)));
}

GL_APICALL GLenum GL_APIENTRY glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    GLES_CTX(GL_WAIT_FAILED);
    auto it = ctx->syncs.find(static_cast<GLuint>(reinterpret_cast<uintptr_t>(sync)));
    if (it == ctx->syncs.end()) {
        set_error(GL_INVALID_VALUE);
        return GL_WAIT_FAILED;
    }
    Device &d = device();
    if (!d.ok) return GL_ALREADY_SIGNALED;
    if (flags & GL_SYNC_FLUSH_COMMANDS_BIT) espgpu_cmd_flush(d.dev, 500);
    const int ms = timeout >= 0xFFFFFFFFull * 1000000ull ? 0x7FFFFFFF : static_cast<int>(timeout / 1000000ull);
    if (espgpu_wait_fence(d.dev, it->second.fence, 0) == 0) return GL_ALREADY_SIGNALED;
    if (ms == 0) return GL_TIMEOUT_EXPIRED;
    return espgpu_wait_fence(d.dev, it->second.fence, ms) == 0 ? GL_CONDITION_SATISFIED : GL_TIMEOUT_EXPIRED;
}

GL_APICALL void GL_APIENTRY glWaitSync(GLsync, GLbitfield, GLuint64)
{
    /* 命令流本来就按顺序执行 */
}

GL_APICALL void GL_APIENTRY glGetSynciv(GLsync sync, GLenum pname, GLsizei count, GLsizei *length, GLint *values)
{
    GLES_CTX();
    if (count < 1) return;
    auto it = ctx->syncs.find(static_cast<GLuint>(reinterpret_cast<uintptr_t>(sync)));
    if (it == ctx->syncs.end()) return set_error(GL_INVALID_VALUE);
    Device &d = device();
    switch (pname) {
    case GL_OBJECT_TYPE: values[0] = GL_SYNC_FENCE; break;
    case GL_SYNC_CONDITION: values[0] = GL_SYNC_GPU_COMMANDS_COMPLETE; break;
    case GL_SYNC_FLAGS: values[0] = 0; break;
    case GL_SYNC_STATUS:
        values[0] = (!d.ok || espgpu_wait_fence(d.dev, it->second.fence, 0) == 0) ? GL_SIGNALED : GL_UNSIGNALED;
        break;
    default: set_error(GL_INVALID_ENUM); return;
    }
    if (length) *length = 1;
}

/* ── 绘制入口（实现在 gl_draw.cpp） ── */

GL_APICALL void GL_APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (first < 0 || count < 0) return set_error(GL_INVALID_VALUE);
    draw(mode, first, count, 0, nullptr, false);
}

GL_APICALL void GL_APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    if (count < 0) return set_error(GL_INVALID_VALUE);
    draw(mode, 0, count, type, indices, true);
}

GL_APICALL void GL_APIENTRY glDrawRangeElements(GLenum mode, GLuint, GLuint, GLsizei count, GLenum type, const void *indices)
{
    glDrawElements(mode, count, type, indices);
}

GL_APICALL void GL_APIENTRY glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount)
{
    if (instancecount != 1) warn_once("instanced draws with instancecount != 1 (drawing one instance)");
    if (instancecount > 0) glDrawArrays(mode, first, count);
}

GL_APICALL void GL_APIENTRY glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices,
                                                    GLsizei instancecount)
{
    if (instancecount != 1) warn_once("instanced draws with instancecount != 1 (drawing one instance)");
    if (instancecount > 0) glDrawElements(mode, count, type, indices);
}

GL_APICALL void GL_APIENTRY glClear(GLbitfield mask)
{
    if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) return set_error(GL_INVALID_VALUE);
    clear(mask);
}

GL_APICALL void GL_APIENTRY glClearBufferfv(GLenum buffer, GLint, const GLfloat *value)
{
    GLES_CTX();
    if (buffer == GL_COLOR) {
        float save[4];
        std::memcpy(save, ctx->clear_color, sizeof(save));
        std::memcpy(ctx->clear_color, value, sizeof(save));
        clear(GL_COLOR_BUFFER_BIT);
        std::memcpy(ctx->clear_color, save, sizeof(save));
    } else if (buffer == GL_DEPTH) {
        const float save = ctx->clear_depth;
        ctx->clear_depth = value[0];
        clear(GL_DEPTH_BUFFER_BIT);
        ctx->clear_depth = save;
    }
}

GL_APICALL void GL_APIENTRY glClearBufferfi(GLenum buffer, GLint, GLfloat depth, GLint)
{
    if (buffer != GL_DEPTH_STENCIL) return set_error(GL_INVALID_ENUM);
    glClearBufferfv(GL_DEPTH, 0, &depth);
}

GL_APICALL void GL_APIENTRY glClearBufferiv(GLenum, GLint, const GLint *) { warn_once("integer clear buffers"); }
GL_APICALL void GL_APIENTRY glClearBufferuiv(GLenum, GLint, const GLuint *) { warn_once("integer clear buffers"); }

GL_APICALL void GL_APIENTRY glFinish(void) { finish(); }
GL_APICALL void GL_APIENTRY glFlush(void) { flush(); }

GL_APICALL void GL_APIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                         void *pixels)
{
    if (width < 0 || height < 0) return set_error(GL_INVALID_VALUE);
    read_pixels(x, y, width, height, format, type, pixels);
}

} // extern "C"
