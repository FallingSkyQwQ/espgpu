/*
 * context.h — libGLESv2 内部状态
 *
 * 一个进程只开一个 espgpu 设备（WinUSB 独占），所有 GL 上下文共用它；
 * 设备侧状态（管线 / 纹理绑定 / 布局 / 矩阵 / 常量寄存器 / 渲染目标）在
 * Device 里做一份镜像，只有真的变化才下发命令，省下行带宽。
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define GL_APICALL
#include <GLES3/gl3.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "espgpu/espgpu.h"
#include "espgpu_glsl.h"

/* libEGL 调用的内部接口（libGLESv2 导出） */
#if defined(ESPGPU_GLES_BUILD)
#define ESPGPU_GLES_INTERNAL extern "C" __declspec(dllexport)
#else
#define ESPGPU_GLES_INTERNAL extern "C" __declspec(dllimport)
#endif

namespace gles {

constexpr int kMaxAttribs = 16;
constexpr int kMaxTexUnits = 8;  /* GL 侧单元数；设备只有 4 个，按着色器实际使用映射 */
constexpr int kDeviceTexUnits = 4;

/* ── 对象 ─────────────────────────────────────────────────────────── */

struct Buffer {
    std::vector<uint8_t> data;
    GLenum usage = GL_STATIC_DRAW;
    bool mapped = false;
    GLintptr map_offset = 0;
    GLsizeiptr map_length = 0;
    GLbitfield map_access = 0;
};

struct Texture {
    GLenum target = 0;
    int width = 0, height = 0;
    GLenum internal_format = GL_RGBA;
    std::vector<uint8_t> rgba;   /**< 主机端母本，RGBA8888 */
    bool has_alpha = true;       /**< 格式本身带 alpha */
    bool luminance = false;      /**< LUMINANCE 一类：设备存 R8 */
    GLenum min_filter = GL_NEAREST_MIPMAP_LINEAR;
    GLenum mag_filter = GL_LINEAR;
    GLenum wrap_s = GL_REPEAT, wrap_t = GL_REPEAT;
    bool dirty = true;           /**< 主机母本有更新，需要重新上传 */
    bool resident = false;       /**< 设备缓存里有 */
    bool device_only = false;    /**< 内容是设备渲染出来的（FBO 目标），主机没有母本 */
    uint32_t dev_bytes = 0;
    uint64_t last_use = 0;
    bool immutable = false;
    int levels = 1;
};

struct Renderbuffer {
    GLenum format = GL_RGBA4;
    int width = 0, height = 0;
    int samples = 0;
};

struct Framebuffer {
    GLuint color_tex = 0;
    GLuint color_rb = 0;
    GLuint depth_rb = 0;
    GLuint depth_tex = 0;
    GLuint stencil_rb = 0;
};

struct Shader {
    GLenum type = 0;
    std::string source;
    bool compiled = false;
    bool compile_attempted = false;
    std::string log;
    std::shared_ptr<espgpu::glsl::Shader> result;
    bool delete_pending = false;
    int attach_count = 0;
};

struct UniformLoc {
    int uniform = -1; /* Program::link.uniforms 下标 */
    int element = 0;
};

struct Program {
    GLuint vs = 0, fs = 0;
    bool linked = false;
    std::string log;
    bool delete_pending = false;
    bool validated = false;

    std::shared_ptr<espgpu::glsl::Shader> vsc, fsc;
    espgpu::glsl::Program link;
    std::vector<std::array<float, 4>> storage; /**< uniform 值，vec4 槽 */
    std::vector<UniformLoc> locations;
    std::map<std::string, GLint> loc_by_name;
    std::map<std::string, GLuint> bound_attribs;  /**< glBindAttribLocation */
    std::vector<int> attrib_loc;                  /**< VS 输入 → GL 属性位置 */
    uint32_t serial = 0;                          /**< 每次链接递增，用于设备程序缓存 */
    uint32_t storage_version = 1;                 /**< uniform 改动计数 */
};

struct AttribArray {
    bool enabled = false;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    bool normalized = false;
    bool integer = false;
    GLsizei stride = 0;
    const void *pointer = nullptr;
    GLuint buffer = 0;
    GLuint divisor = 0;
};

struct VertexArray {
    AttribArray attribs[kMaxAttribs];
    GLuint element_buffer = 0;
};

struct Sync {
    uint32_t fence = 0;
};

/** 可在上下文之间共享的对象名字空间 */
struct Shared {
    std::map<GLuint, Buffer> buffers;
    std::map<GLuint, Texture> textures;
    std::map<GLuint, Renderbuffer> renderbuffers;
    std::map<GLuint, std::unique_ptr<Shader>> shaders;
    std::map<GLuint, std::unique_ptr<Program>> programs;
    GLuint next_buffer = 1, next_texture = 1, next_rb = 1, next_object = 1;
};

/* ── 设备状态镜像 ─────────────────────────────────────────────────── */

struct Device {
    espgpu_device_t *dev = nullptr;
    bool ok = false;
    int width = 320, height = 200;
    uint32_t tex_budget = 4u << 20;
    uint32_t max_tex_dim = 512;
    uint64_t frame_counter = 1;
    uint32_t last_frame_id = 0;
    uint32_t fence_seq = 0;

    /* 镜像（valid=false 时下一次一定下发） */
    bool pipe_valid = false;
    espgpu_pipeline_t pipe{};
    espgpu_pipeline_ext_t pipe_ext{};
    bool layout_valid = false;
    espgpu_vertex_layout_t layout{};
    bool mvp_valid = false;
    float mvp[16] = {};
    bool vp_valid = false;
    espgpu_rect_t viewport{};
    bool sc_valid = false;
    espgpu_rect_t scissor{};
    bool dr_valid = false;
    float depth_near = 0, depth_far = 1;
    bool color_valid = false;
    uint8_t color[4] = {};
    struct UnitBind {
        bool valid = false;
        uint32_t id = 0;
        uint8_t filter = 0, wrap = 0, wrap_t = 0;
    } units[kDeviceTexUnits];
    uint32_t target_tex = 0; /**< 当前渲染目标：0 = 屏幕 */
    int target_w = 0, target_h = 0;
    bool target_valid = false;

    /* 片元程序缓存：设备 16 个槽 */
    struct ProgSlot {
        const Program *prog = nullptr;
        uint32_t serial = 0;
        uint64_t last_use = 0;
    } prog_slots[ESPGPU_VM_MAX_PROGRAMS];
    float consts[ESPGPU_VM_CONSTS][4] = {};
    int consts_count = -1;

    uint32_t resident_bytes = 0;

    void invalidate()
    {
        pipe_valid = layout_valid = mvp_valid = vp_valid = sc_valid = dr_valid = color_valid = false;
        target_valid = false;
        for (auto &u : units) u.valid = false;
        for (auto &p : prog_slots) p = ProgSlot{};
        consts_count = -1;
    }
};

Device &device();
bool device_open();
void device_close();

/* ── 上下文 ───────────────────────────────────────────────────────── */

struct Context {
    std::shared_ptr<Shared> sh;
    int version = 3;

    /* 窗口表面 */
    HWND hwnd = nullptr;
    int win_w = 320, win_h = 200;
    int swap_interval = 1;

    GLenum error = GL_NO_ERROR;

    /* 绑定 */
    GLuint array_buffer = 0;
    GLuint copy_read_buffer = 0, copy_write_buffer = 0;
    GLuint pixel_pack_buffer = 0, pixel_unpack_buffer = 0;
    GLuint vao = 0;
    std::map<GLuint, VertexArray> vaos; /* 0 = 默认 VAO */
    GLuint next_vao = 1;
    GLuint program = 0;
    GLuint renderbuffer = 0;
    GLuint draw_fbo = 0, read_fbo = 0;
    std::map<GLuint, Framebuffer> fbos;
    GLuint next_fbo = 1;
    int active_texture = 0;
    GLuint tex2d[kMaxTexUnits] = {};
    std::array<float, 4> generic_attr[kMaxAttribs];

    /* 状态 */
    bool blend = false, depth_test = false, cull = false, scissor_test = false;
    bool polygon_offset = false, dither = true, stencil_test = false;
    GLenum blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO;
    GLenum blend_src_a = GL_ONE, blend_dst_a = GL_ZERO;
    GLenum blend_eq_rgb = GL_FUNC_ADD, blend_eq_a = GL_FUNC_ADD;
    float blend_color[4] = {0, 0, 0, 0};
    GLenum depth_func = GL_LESS;
    bool depth_mask = true;
    float depth_near = 0, depth_far = 1;
    GLenum cull_face = GL_BACK;
    GLenum front_face = GL_CCW;
    bool color_mask[4] = {true, true, true, true};
    float clear_color[4] = {0, 0, 0, 0};
    float clear_depth = 1;
    GLint clear_stencil = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint scissor[4] = {0, 0, 0, 0};
    float line_width = 1;
    float poly_factor = 0, poly_units = 0;
    GLint unpack_alignment = 4, pack_alignment = 4;
    GLint unpack_row_length = 0, unpack_skip_rows = 0, unpack_skip_pixels = 0;
    GLint pack_row_length = 0, pack_skip_rows = 0, pack_skip_pixels = 0;
    GLenum hint_mipmap = GL_DONT_CARE;
    GLenum read_buffer = GL_BACK;

    /* stencil：设备不支持，只保存状态供查询 */
    GLenum stencil_func = GL_ALWAYS;
    GLint stencil_ref = 0;
    GLuint stencil_value_mask = 0xFFFFFFFFu, stencil_writemask = 0xFFFFFFFFu;
    GLenum stencil_fail = GL_KEEP, stencil_zfail = GL_KEEP, stencil_zpass = GL_KEEP;

    std::map<GLuint, Sync> syncs;
    GLuint next_sync = 1;

    std::set<std::string> warned;

    bool viewport_set = false;

    Context()
    {
        for (auto &g : generic_attr) g = {0, 0, 0, 1};
        vaos[0] = VertexArray{};
    }

    VertexArray &cur_vao() { return vaos[vao]; }
};

Context *current();
void set_current(Context *c);

void set_error(GLenum e);
void warn_once(const char *what);

/* ── 绘制路径（gl_draw.cpp） ── */
void draw(GLenum mode, GLint first, GLsizei count, GLenum itype, const void *indices, bool indexed);
void clear(GLbitfield mask);
void swap_buffers();
void finish();
void flush();
void copy_tex_image(GLuint tex, GLint x, GLint y, GLsizei w, GLsizei h);
void read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *out);
void texture_deleted(GLuint id);

/* ── 工具 ── */
bool unpack_to_rgba(const Context &c, int w, int h, GLenum format, GLenum type, const void *src,
                    std::vector<uint8_t> &out, bool &has_alpha, bool &luminance);

} // namespace gles

#define GLES_CTX(ret)                                                                          \
    gles::Context *ctx = gles::current();                                                      \
    if (!ctx) return ret
