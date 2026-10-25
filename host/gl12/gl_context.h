/*
 * gl_context.h — GL 1.2 固定管线的状态机
 *
 * 结构：
 *   - Context        每个 HGLRC 一份：管线状态、矩阵栈、客户端数组、立即模式、待发批次
 *   - Shared         进程内共享：纹理对象、显示列表、设备连接（简化：所有上下文共用一个命名空间，
 *                    等价于全部 wglShareLists）
 *
 * 绘制路径：
 *   glBegin/glEnd、glDrawArrays/Elements、glCallList 都汇成 emit()，追加进 Pending 批次。
 *   状态变化只做标记（state_changed）；出几何时 begin_geometry 把当前状态解析成 DrawKey，
 *   与批次的比较：相同就继续合批，不同就另开批次（可重排的旧批次暂存在 parked 里，之后同状态的几何还能并回去）。
 *   每个批次带着开始时的状态快照和矩阵，flush 时换上快照同步设备（视口/裁剪/管线/纹理/光照），
 *   按 PLAN §2 的带宽预算把顶点压成 12 字节左右，再 espgpu_cmd_draw。
 *   改纹理数据 / 清屏 / 呈现等是顺序屏障：flush_pending 把所有批次按开始的先后发出。
 */

#pragma once

#include "gl_dispatch.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "espgpu/espgpu.h"

#ifndef GL_COMBINE
#define GL_COMBINE 0x8570
#endif

namespace espgl {

constexpr int kMaxUnits = 2;       /* 设备着色模式最多折叠两层纹理 */
constexpr int kMaxLights = 8;      /* GL 侧；设备只用前 ESPGPU_MAX_LIGHTS 盏 */
constexpr int kModelviewDepth = 32;
constexpr int kProjectionDepth = 4;
constexpr int kTextureDepth = 4;
constexpr int kAttribDepth = 16;

/* ── 数学 ─────────────────────────────────────────────────────────── */

/** 4x4 矩阵，GL 习惯的列主序：m[col*4+row] */
struct Mat4 {
    double m[16];
    static Mat4 identity()
    {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0;
        return r;
    }
    bool is_identity() const
    {
        static const Mat4 id = identity();
        return memcmp(m, id.m, sizeof(m)) == 0;
    }
};

Mat4 mat_mul(const Mat4 &a, const Mat4 &b);
bool mat_inverse(const Mat4 &a, Mat4 &out);
void mat_xform(const Mat4 &a, const float in[4], float out[4]);

struct MatStack {
    std::vector<Mat4> s;
    size_t max_depth = 2;
    MatStack() { s.push_back(Mat4::identity()); }
    explicit MatStack(size_t depth) : max_depth(depth) { s.push_back(Mat4::identity()); }
    Mat4 &top() { return s.back(); }
    const Mat4 &top() const { return s.back(); }
};

/* ── 顶点 ─────────────────────────────────────────────────────────── */

struct Vertex {
    float pos[4];
    float color[4];
    float tc[kMaxUnits][4];
    float normal[3];
};

/* ── 状态 ─────────────────────────────────────────────────────────── */

struct TexUnitState {
    bool enable_2d = false;
    bool enable_1d = false;
    GLuint bound_2d = 0;
    GLuint bound_1d = 0;
    GLenum env_mode = GL_MODULATE;
    float env_color[4] = {0, 0, 0, 0};
    GLenum combine_rgb = GL_MODULATE;
    GLenum combine_alpha = GL_MODULATE;
    GLenum src_rgb[3] = {GL_TEXTURE, 0x8578 /*GL_PREVIOUS*/, 0x8576 /*GL_CONSTANT*/};
    GLenum op_rgb[3] = {GL_SRC_COLOR, GL_SRC_COLOR, GL_SRC_ALPHA};
    GLenum src_alpha[3] = {GL_TEXTURE, 0x8578, 0x8576};
    GLenum op_alpha[3] = {GL_SRC_ALPHA, GL_SRC_ALPHA, GL_SRC_ALPHA};
    float rgb_scale = 1.0f;
    float alpha_scale = 1.0f;
    float lod_bias = 0.0f;
    bool texgen[4] = {false, false, false, false};
};

struct LightState {
    bool enabled = false;
    float ambient[4] = {0, 0, 0, 1};
    float diffuse[4] = {0, 0, 0, 1};
    float specular[4] = {0, 0, 0, 1};
    float position[4] = {0, 0, 1, 0}; /* 眼空间 */
    float spot_dir[3] = {0, 0, -1};
    float spot_exp = 0, spot_cutoff = 180;
    float atten[3] = {1, 0, 0};
};

struct MaterialState {
    float ambient[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    float diffuse[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    float specular[4] = {0, 0, 0, 1};
    float emission[4] = {0, 0, 0, 1};
    float shininess = 0;
};

/** 服务端状态，按 glPushAttrib 的分组组织 */
struct State {
    /* GL_COLOR_BUFFER_BIT */
    bool blend = false;
    GLenum blend_src = GL_ONE, blend_dst = GL_ZERO, blend_eq = 0x8006 /*GL_FUNC_ADD*/;
    float blend_color[4] = {0, 0, 0, 0};
    bool alpha_test = false;
    GLenum alpha_func = GL_ALWAYS;
    float alpha_ref = 0;
    bool color_mask[4] = {true, true, true, true};
    float clear_color[4] = {0, 0, 0, 0};
    bool dither = true;
    bool logic_op = false;
    GLenum logic_op_mode = GL_COPY;
    GLenum draw_buffer = GL_BACK;

    /* GL_DEPTH_BUFFER_BIT */
    bool depth_test = false;
    GLenum depth_func = GL_LESS;
    bool depth_mask = true;
    double clear_depth = 1.0;

    /* GL_STENCIL_BUFFER_BIT（不支持，只保存） */
    bool stencil_test = false;
    GLenum stencil_func = GL_ALWAYS;
    GLint stencil_ref = 0;
    GLuint stencil_mask = 0xFFFFFFFFu, stencil_writemask = 0xFFFFFFFFu;
    GLenum stencil_fail = GL_KEEP, stencil_zfail = GL_KEEP, stencil_zpass = GL_KEEP;
    GLint clear_stencil = 0;

    /* GL_POLYGON_BIT */
    bool cull = false;
    GLenum cull_face = GL_BACK;
    GLenum front_face = GL_CCW;
    GLenum polygon_mode[2] = {GL_FILL, GL_FILL};
    bool polygon_offset_fill = false, polygon_offset_line = false, polygon_offset_point = false;
    float offset_factor = 0, offset_units = 0;
    bool polygon_smooth = false, polygon_stipple = false;

    /* GL_LIGHTING_BIT */
    bool lighting = false;
    GLenum shade_model = GL_SMOOTH;
    bool color_material = false;
    GLenum color_material_face = GL_FRONT_AND_BACK;
    GLenum color_material_mode = GL_AMBIENT_AND_DIFFUSE;
    float light_model_ambient[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    bool light_model_two_side = false;
    bool light_model_local_viewer = false;
    LightState lights[kMaxLights];
    MaterialState material[2]; /* 0 front 1 back */

    /* GL_FOG_BIT */
    bool fog = false;
    GLenum fog_mode = GL_EXP;
    float fog_density = 1, fog_start = 0, fog_end = 1;
    float fog_color[4] = {0, 0, 0, 0};
    float fog_index = 0;

    /* GL_LINE_BIT / GL_POINT_BIT */
    float line_width = 1;
    bool line_smooth = false, line_stipple = false;
    GLint line_stipple_factor = 1;
    GLushort line_stipple_pattern = 0xFFFF;
    float point_size = 1;
    bool point_smooth = false;

    /* GL_SCISSOR_BIT */
    bool scissor = false;
    GLint scissor_box[4] = {0, 0, 0, 0};

    /* GL_VIEWPORT_BIT */
    GLint viewport[4] = {0, 0, 0, 0};
    double depth_range[2] = {0.0, 1.0};

    /* GL_TEXTURE_BIT */
    TexUnitState unit[kMaxUnits];
    int active_unit = 0;

    /* GL_TRANSFORM_BIT */
    GLenum matrix_mode = GL_MODELVIEW;
    bool normalize = false, rescale_normal = false;
    bool clip_plane[6] = {};

    /* GL_HINT_BIT */
    GLenum hint_perspective = GL_DONT_CARE, hint_fog = GL_DONT_CARE;
    GLenum hint_line = GL_DONT_CARE, hint_point = GL_DONT_CARE, hint_polygon = GL_DONT_CARE;

    /* GL_CURRENT_BIT */
    float cur_color[4] = {1, 1, 1, 1};
    float cur_normal[3] = {0, 0, 1};
    float cur_tc[kMaxUnits][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    float raster_pos[4] = {0, 0, 0, 1};
    float raster_color[4] = {1, 1, 1, 1};
    bool raster_valid = true;
    bool edge_flag = true;

    /* GL_PIXEL_MODE_BIT（部分） */
    float zoom_x = 1, zoom_y = 1;
    GLenum read_buffer = GL_BACK;

    /* GL_LIST_BIT */
    GLuint list_base = 0;

    /* 其它 */
    bool shared_palette = false;
};

struct ArrayState {
    bool enabled = false;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLsizei stride = 0;
    const void *ptr = nullptr;
};

struct PixelStore {
    GLint alignment = 4;
    GLint row_length = 0;
    GLint skip_rows = 0;
    GLint skip_pixels = 0;
    bool swap_bytes = false;
    bool lsb_first = false;
};

struct ClientState {
    ArrayState vertex, normal, color, index, edge;
    ArrayState tc[kMaxUnits];
    int client_active = 0;
    PixelStore unpack, pack;
};

/* ── 纹理对象 ─────────────────────────────────────────────────────── */

struct Texture {
    GLuint name = 0;
    uint16_t dev_id = 0;
    GLenum target = GL_TEXTURE_2D;

    /* GL 层级 0 的原始数据（未缩放），供 SubImage 与重传 */
    int w = 0, h = 0;
    GLint internal_format = GL_RGBA;
    std::vector<uint8_t> rgba;    /* RGBA8，非调色板 */
    std::vector<uint8_t> index;   /* 调色板纹理的索引 */
    bool paletted = false;

    /* 调色板（GL_EXT_paletted_texture） */
    bool has_palette = false;
    uint8_t palette[256 * 4] = {};
    int palette_size = 0;
    GLenum palette_format = GL_RGBA;

    /* 设备形态 */
    uint8_t dev_fmt = ESPGPU_FMT_RGB565;
    int dw = 0, dh = 0;
    std::vector<uint8_t> dev_pixels;
    uint32_t dev_bytes = 0;
    bool has_image = false;
    bool dirty = true;        /* dev_pixels 需要重新生成 */
    bool upload = true;       /* 需要上传到设备 */
    bool resident = false;
    bool device_only = false; /* glCopyTexImage 产生的内容只存在设备上 */
    bool pal_changed = false; /* 调色板改过、设备上的还是旧的（不能走「图像没变就不重传」的捷径） */
    uint64_t last_used = 0;
    float priority = 1.0f;
    bool pin = false;         /* 当前这次绘制正要用：LRU 绝不换出 */
    bool prioritized = false; /* glPrioritizeTextures 显式给过高优先级 → ESPGPU_TEXF_PRIORITY */
    int level_w[12] = {}, level_h[12] = {}; /* 只用层级 0，其余层级只记尺寸供查询 */

    /* 采样参数 */
    GLenum min_filter = GL_NEAREST_MIPMAP_LINEAR;
    GLenum mag_filter = GL_LINEAR;
    GLenum wrap_s = GL_REPEAT, wrap_t = GL_REPEAT;
    float border_color[4] = {0, 0, 0, 0};
};

/* ── 显示列表 ─────────────────────────────────────────────────────── */

struct ListBatch {
    GLenum mode = GL_TRIANGLES;
    std::vector<Vertex> v;
    bool has_color = false;
    bool has_tc[kMaxUnits] = {false, false};
    bool has_normal = false;
    uint32_t base_vertex = 0; /* 在列表设备缓冲里的顶点偏移 */
};

struct ListOp {
    int kind = 0; /* 0 = 状态闭包，1 = 几何批次 */
    std::function<void()> fn;
    int batch = -1;
};

/** 显示列表里连续的三角形类几何（中间没有状态命令）：缓存后合成一条带索引的 DRAW */
struct ListRun {
    int first_op = 0, last_op = 0; /* ops 下标，闭区间 */
    uint32_t base_vertex = 0, vertex_count = 0;
    uint32_t index_offset = 0, index_count = 0;
};

struct DisplayList {
    std::vector<ListOp> ops;
    std::vector<ListBatch> batches;

    /* 设备端缓存（R6） */
    uint32_t buffer_id = 0;
    bool buffer_valid = false;
    bool cacheable = false;
    espgpu_vertex_layout_t layout{};
    double origin[3] = {0, 0, 0};
    bool pos_f32 = false;
    uint32_t replays = 0;
    std::vector<ListRun> runs;
    std::vector<int> run_at;   /* ops 下标 → runs 下标（-1 = 不是某个 run 的开头） */
    uint32_t buffer_bytes = 0;
    uint64_t last_used = 0;    /* 帧号，设备缓冲 LRU 用 */
    int evictions = 0;         /* 被设备换出的次数：反复换出就不再缓存 */
};

/** 正在录制的列表 */
struct Recorder {
    GLuint name = 0;
    GLenum mode = GL_COMPILE;
    std::unique_ptr<DisplayList> list;
    bool color_set = false, normal_set = false;
    bool tc_set[kMaxUnits] = {false, false};
    bool in_begin = false;
    ListBatch cur;
};

/* ── 待发批次 ─────────────────────────────────────────────────────── */

/** GL 状态解析到设备侧的结果（resolve_key）。相等 = 对设备没区别，几何可以合批。 */
struct DrawKey {
    espgpu_pipeline_t p;
    espgpu_pipeline_fog_t fog;
    espgpu_pipeline_ext_t ext;
    int16_t vp[4], sc[4];
    float dr[2];
    const Texture *tex[kMaxUnits];
    int units[kMaxUnits];
    uint8_t filt[kMaxUnits], wrap[kMaxUnits];
    int nu;
    bool white;
    bool reorder_ok; /* 不透明 + 深度测试写深度：和同类批次之间可以重排 */
    uint64_t light_sig;
};

enum PrimClass {
    PCLS_NONE = 0,
    PCLS_QUADS = 1,  /* ESPGPU_PRIM_QUADS，无索引 */
    PCLS_TRIS = 2,   /* TRIANGLES + u16 索引 */
    PCLS_LINES = 3,  /* LINES + u16 索引 */
    PCLS_POINTS = 4, /* POINTS，无索引 */
};

struct Pending {
    /* 批次开始时的状态快照：flush 用它（而不是 flush 时刻的当前状态）同步设备。
     * 状态变化不必立刻 flush——下一次出几何时再比较 DrawKey，对设备没区别就继续合批。 */
    State st;
    DrawKey key{};
    uint64_t seq = 0;
    /* 批次所在的变换：flush 用它算 MVP（而不是 flush 时刻的当前矩阵）。
     * 之后来的几何若只是 modelview 差一个仿射变换，就在主机上把顶点变换进本批次的坐标系，继续合批。 */
    Mat4 mv = Mat4::identity(), proj = Mat4::identity();
    Mat4 mv_inv = Mat4::identity();
    bool mv_inv_ok = false;
    std::vector<Vertex> quads;
    std::vector<Vertex> tri_v;
    std::vector<uint16_t> tri_i;
    std::vector<Vertex> line_v;
    std::vector<uint16_t> line_i;
    std::vector<Vertex> points;
    int classes = 0; /* 位掩码 1 << PrimClass */
    size_t total() const { return quads.size() + tri_v.size() + line_v.size() + points.size(); }
    bool empty() const { return classes == 0; }
    void clear()
    {
        quads.clear();
        tri_v.clear();
        tri_i.clear();
        line_v.clear();
        line_i.clear();
        points.clear();
        classes = 0;
        mv_inv_ok = false;
    }
};

/* ── 设备同步缓存 ─────────────────────────────────────────────────── */

struct DevCache {
    bool valid = false;
    espgpu_pipeline_t pipe{};
    espgpu_pipeline_fog_t fog{};
    espgpu_pipeline_ext_t ext{};
    espgpu_vertex_layout_t layout{};
    bool layout_valid = false;
    float mvp[16] = {};
    bool mvp_valid = false;
    double origin[3] = {0, 0, 0}; /* 设备 MVP 里折进去的批次原点（I16 位置的量化坐标系） */
    bool origin_valid = false;
    float normal_m[16] = {};
    bool normal_valid = false;
    uint8_t color[4] = {255, 255, 255, 255};
    bool color_valid = false;
    int16_t viewport[4] = {-1, -1, -1, -1};
    int16_t scissor[4] = {-1, -1, -1, -1};
    float depth_range[2] = {-1, -1};
    std::vector<uint8_t> lighting;
    uint16_t bound[ESPGPU_MAX_TEXTURE_UNITS] = {};
    uint8_t bound_filter[ESPGPU_MAX_TEXTURE_UNITS] = {};
    uint8_t bound_wrap[ESPGPU_MAX_TEXTURE_UNITS] = {};
    bool bound_valid = false;
    uint32_t bind_epoch = 0; /* 与 Shared::bind_epoch 不同则重新绑定（纹理被重传 / 换出过） */
};

/* ── 上下文 ───────────────────────────────────────────────────────── */

struct AttribEntry {
    GLbitfield mask;
    State st;
};

struct ClientAttribEntry {
    GLbitfield mask;
    ClientState cs;
};

struct Context {
    HDC hdc = nullptr;
    HWND hwnd = nullptr;
    int win_w = 1, win_h = 1;   /* 客户区尺寸 */
    bool viewport_init = false;

    State st;
    ClientState cs;
    std::vector<AttribEntry> attrib_stack;
    std::vector<ClientAttribEntry> client_attrib_stack;

    MatStack modelview{kModelviewDepth};
    MatStack projection{kProjectionDepth};
    MatStack texmat[kMaxUnits] = {MatStack(kTextureDepth), MatStack(kTextureDepth)};
    uint32_t mv_serial = 1;     /* modelview / projection 变化计数，用于缓存 MVP */

    GLenum error = GL_NO_ERROR;

    /* 立即模式 */
    bool in_begin = false;
    GLenum begin_mode = GL_POINTS;
    std::vector<Vertex> im;

    /* 显示列表录制 / 回放 */
    std::unique_ptr<Recorder> rec;
    int replay_depth = 0;

    Pending pending;                 /* 正在接收几何的批次 */
    std::vector<Pending> parked;     /* 暂存的可重排批次（DrawKey 各不相同），按 seq 顺序发出 */
    bool key_dirty = true;           /* 状态变过：下一次出几何前要重新解析 DrawKey */
    uint64_t batch_seq = 0;
    std::vector<uint8_t> scratch_blob;
    DevCache dc;

    /* glLockArraysEXT */
    bool arrays_locked = false;

    int swap_interval = 1;

    /* wglMakeCurrent：当前绑在哪个线程上（0 = 没有）。WGL 规定一个上下文同时只能在一个线程上当前 */
    DWORD owner_thread = 0;
};

/* ── 共享对象 ─────────────────────────────────────────────────────── */

struct Shared {
    std::recursive_mutex mu;
    std::unordered_map<GLuint, std::unique_ptr<Texture>> textures;
    std::unordered_map<uint16_t, GLuint> dev_to_name;
    std::vector<uint16_t> free_dev_ids;
    uint16_t next_dev_id = 1;
    GLuint next_tex_name = 1;
    Texture default_tex; /* 名字 0 */

    std::map<GLuint, std::unique_ptr<DisplayList>> lists;
    uint32_t next_buffer_id = 1;
    std::unordered_map<uint32_t, GLuint> buffer_to_list;
    uint32_t list_buffers = 0;       /* 设备上驻留的列表缓冲个数 / 字节（设备最多 256 个、固件预算 512 KB） */
    uint64_t list_buffer_bytes = 0;

    /* 共享调色板（GL_SHARED_TEXTURE_PALETTE_EXT） */
    uint8_t shared_palette[256 * 4] = {};
    int shared_palette_size = 0;

    /* 设备 */
    espgpu_device_t *dev = nullptr;
    bool dev_tried = false;
    int dev_w = 320, dev_h = 200;
    int fps = 24;
    uint32_t max_tex_dim = 512;
    uint64_t tex_budget = 4u * 1024 * 1024;
    uint64_t tex_used = 0;
    uint64_t frame = 1;
    uint32_t last_frame_id = 0;
    /** 上一帧已提交但还没确认回传：下一次 PRESENT 前先把它等掉，避免设备慢时无限排队 */
    bool inflight = false;
    uint32_t inflight_before = 0;
    uint32_t fence = 0;
    LARGE_INTEGER qpf{}, next_frame_qpc{};
    bool compress = true;
    uint32_t bind_epoch = 1;
    std::vector<Context *> contexts;
    Context *dc_owner = nullptr; /* 最近一次同步设备状态的上下文；换了人就整套重发 */
};

extern Shared g_sh;

/** 当前线程的上下文（espgpu 模式） */
Context *cur();
void set_cur(Context *c);

/* ── 跨文件的内部接口 ─────────────────────────────────────────────── */

void set_error(GLenum e);

/* 设备 */
bool device_open();
void device_poll_events();
void device_present(Context *c);

/* 绘制 */
void emit(Context *c, GLenum mode, const Vertex *v, size_t n);
void emit_indexed(Context *c, GLenum mode, const Vertex *v, size_t nv, const uint32_t *idx, size_t ni);
void flush_pending(Context *c);        /* 把所有待发批次（含暂存的）按顺序发出：命令顺序屏障 */
void state_dirty(Context *c);          /* 即将改纹理数据之类：批次引用的东西要变，先全部 flush */
void state_changed(Context *c);        /* 纯状态变化：只标记，下一次出几何时比较 DrawKey */
void begin_geometry(Context *c);       /* 出几何前调用：按当前状态选 / 开批次 */
void flush_current(Context *c);        /* 只发当前批次（它和暂存批次之间可重排时用） */
void resolve_key(Context *c, DrawKey &k);
bool key_equal(const DrawKey &a, const DrawKey &b);
bool sync_device_state(Context *c, int *shade_out, bool *force_white, int dev_units[2], int *n_units);
void sync_viewport_scissor(Context *c);
void fetch_array_vertex(Context *c, GLint i, Vertex &out);
void current_vertex(Context *c, Vertex &v); /* 用当前属性填充（不含位置） */

/** 设备坐标：窗口坐标（左下角原点）→ 设备矩形（左上角原点） */
void win_rect_to_dev(Context *c, GLint x, GLint y, GLint w, GLint h, int16_t out[4]);

/* 纹理 */
Texture *tex_lookup(GLuint name, bool create);
Texture *tex_bound(Context *c, int unit);
bool tex_usable(const Texture *t);
bool tex_make_resident(Texture *t);
void tex_on_evicted(uint16_t dev_id);
uint8_t tex_dev_filter(const Texture *t);
uint8_t tex_dev_wrap(const Texture *t);
void tex_unpack_to_rgba(Context *c, int w, int h, GLenum format, GLenum type, const void *pixels,
                        std::vector<uint8_t> &rgba, std::vector<uint8_t> *index_out);
void tex_pack_from_rgba(Context *c, int w, int h, GLenum format, GLenum type, const uint8_t *rgba,
                        void *pixels);

/* 显示列表 */
bool list_recording(Context *c);
bool list_compile_only(Context *c);
void list_record_fn(Context *c, std::function<void()> fn);
void list_on_evicted(uint32_t buffer_id);

/* 杂项 */
Mat4 &cur_matrix(Context *c);
void matrix_changed(Context *c);
void begin_frame_bookkeeping();
int env_int(const char *name, int def);

/** 录制宏：在 impl 开头使用。编译模式下只录制不执行。 */
#define ESPGL_RECORD(ctx, ...)                                                                    \
    do {                                                                                          \
        if (list_recording(ctx)) {                                                                \
            list_record_fn(ctx, [=]() { __VA_ARGS__; });                                         \
            if (list_compile_only(ctx)) {                                                         \
                return;                                                                           \
            }                                                                                     \
        }                                                                                         \
    } while (0)

/** 取当前上下文，没有就直接返回 */
#define ESPGL_CTX(c)                                                                              \
    Context *c = cur();                                                                           \
    if (c == nullptr) {                                                                           \
        return;                                                                                   \
    }

#define ESPGL_CTX_RET(c, v)                                                                       \
    Context *c = cur();                                                                           \
    if (c == nullptr) {                                                                           \
        return v;                                                                                 \
    }

} // namespace espgl
