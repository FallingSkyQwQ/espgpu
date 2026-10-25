/*
 * gl_list.cpp — 显示列表（PLAN R6：静态几何缓存到设备端缓冲）
 *
 * 录制：状态命令记成闭包，glBegin/glEnd 与顶点数组记成 ListBatch（顶点按 GL 语义在录制时取值）。
 * 回放：列表被调用到第 2 次起，若当前状态允许（见 can_use_buffer），把整张列表的几何
 *       一次性编码上传成一个设备缓冲（布局、原点在整张列表内统一），之后每个批次只发
 *       一条 ~24 字节的 DRAW(VSRC_BUFFER)，顶点不再走 USB。
 */

#include "gl_context.h"

#include <algorithm>

namespace espgl {

void recorder_reset_attrs(Context *c);
bool list_plan_encode(Context *c, std::vector<Vertex> &all, bool want_color, bool want_tc[2], bool want_normal,
                      espgpu_vertex_layout_t &L, double origin[3], std::vector<uint8_t> &blob);
void send_layout_and_matrix(Context *c, const espgpu_vertex_layout_t &L, const double origin[3], bool const_color,
                            const uint8_t cc[4]);

namespace {

constexpr int kMaxNesting = 64;
constexpr size_t kMinCachedVerts = 32;

uint8_t dev_prim(GLenum mode)
{
    switch (mode) {
    case GL_POINTS: return ESPGPU_PRIM_POINTS;
    case GL_LINES: return ESPGPU_PRIM_LINES;
    case GL_LINE_STRIP: return ESPGPU_PRIM_LINE_STRIP;
    case GL_LINE_LOOP: return ESPGPU_PRIM_LINE_LOOP;
    case GL_TRIANGLES: return ESPGPU_PRIM_TRIANGLES;
    case GL_TRIANGLE_STRIP:
    case GL_QUAD_STRIP: return ESPGPU_PRIM_TRIANGLE_STRIP; /* 顶点顺序与 quad strip 相同 */
    case GL_TRIANGLE_FAN:
    case GL_POLYGON: return ESPGPU_PRIM_TRIANGLE_FAN;
    case GL_QUADS: return ESPGPU_PRIM_QUADS;
    default: return ESPGPU_PRIM_TRIANGLES;
    }
}

uint8_t to8(float v)
{
    return static_cast<uint8_t>(std::lround((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f));
}

/** 整张列表的属性是否一致（统一布局的前提） */
void analyse(DisplayList &L)
{
    size_t total = 0;
    bool first = true;
    bool col = false, nrm = false, tc[kMaxUnits] = {false, false};
    L.cacheable = true;
    for (const ListBatch &b : L.batches) {
        total += b.v.size();
        if (first) {
            col = b.has_color;
            nrm = b.has_normal;
            for (int u = 0; u < kMaxUnits; u++) tc[u] = b.has_tc[u];
            first = false;
            continue;
        }
        if (b.has_color != col || b.has_normal != nrm) {
            L.cacheable = false;
        }
        for (int u = 0; u < kMaxUnits; u++) {
            if (b.has_tc[u] != tc[u]) L.cacheable = false;
        }
    }
    if (total < kMinCachedVerts || L.batches.empty()) {
        L.cacheable = false;
    }
}

/** 当前状态下能不能直接用设备缓冲里的顶点 */
bool can_use_buffer(Context *c, const DisplayList &L, int shade, bool white, const int dev_units[2], int nu)
{
    (void)shade;
    if (!L.cacheable || L.batches.empty()) {
        return false;
    }
    const ListBatch &b0 = L.batches.front();
    const State &s = c->st;
    for (int k = 0; k < nu; k++) {
        if (dev_units[k] != k) {
            return false; /* 缓冲里 TEXCOORDu 对应 GL 单元 u，必须一一对应 */
        }
        if (!b0.has_tc[k]) {
            return false; /* 常量纹理坐标没法放进设备缓冲 */
        }
        if (!c->texmat[k].top().is_identity()) {
            return false;
        }
    }
    const bool lit_const = s.lighting && !(s.color_material && (s.color_material_mode == GL_DIFFUSE ||
                                                                 s.color_material_mode == GL_AMBIENT_AND_DIFFUSE));
    if (b0.has_color && (white || lit_const)) {
        return false;
    }
    if (s.lighting && !b0.has_normal) {
        return false;
    }
    return true;
}

bool ensure_buffer(DisplayList &L, Context *c)
{
    if (L.buffer_valid) {
        return true;
    }
    if (g_sh.dev == nullptr) {
        return false;
    }
    std::vector<Vertex> all;
    uint32_t base = 0;
    for (ListBatch &b : L.batches) {
        b.base_vertex = base;
        base += static_cast<uint32_t>(b.v.size());
        all.insert(all.end(), b.v.begin(), b.v.end());
    }
    const ListBatch &b0 = L.batches.front();
    bool want_tc[2] = {b0.has_tc[0], b0.has_tc[1]};
    std::vector<uint8_t> blob;
    if (!list_plan_encode(c, all, b0.has_color, want_tc, b0.has_normal, L.layout, L.origin, blob)) {
        L.cacheable = false;
        return false;
    }
    if (L.buffer_id == 0) {
        L.buffer_id = g_sh.next_buffer_id++;
    }
    if (espgpu_buffer_upload(g_sh.dev, L.buffer_id, blob.data(), static_cast<uint32_t>(blob.size())) != 0) {
        L.cacheable = false;
        return false;
    }
    L.buffer_valid = true;
    return true;
}

void replay_batch_inline(Context *c, const ListBatch &b)
{
    std::vector<Vertex> v = b.v;
    for (Vertex &x : v) {
        if (!b.has_color) memcpy(x.color, c->st.cur_color, sizeof(x.color));
        if (!b.has_normal) memcpy(x.normal, c->st.cur_normal, sizeof(x.normal));
        for (int u = 0; u < kMaxUnits; u++) {
            if (!b.has_tc[u]) memcpy(x.tc[u], c->st.cur_tc[u], sizeof(x.tc[u]));
        }
    }
    emit(c, b.mode, v.data(), v.size());
}

void after_batch(Context *c, const ListBatch &b)
{
    if (b.v.empty()) {
        return;
    }
    const Vertex &last = b.v.back();
    if (b.has_color) memcpy(c->st.cur_color, last.color, sizeof(last.color));
    if (b.has_normal) memcpy(c->st.cur_normal, last.normal, sizeof(last.normal));
    for (int u = 0; u < kMaxUnits; u++) {
        if (b.has_tc[u]) memcpy(c->st.cur_tc[u], last.tc[u], sizeof(last.tc[u]));
    }
}

void execute(Context *c, GLuint name, int depth)
{
    if (depth > kMaxNesting) {
        return;
    }
    auto it = g_sh.lists.find(name);
    if (it == g_sh.lists.end()) {
        return;
    }
    DisplayList &L = *it->second;
    L.replays++;

    /* 回放期间暂停录制（COMPILE_AND_EXECUTE 下 CallList 本身已经被录了） */
    std::unique_ptr<Recorder> saved = std::move(c->rec);
    c->replay_depth = depth;

    for (const ListOp &op : L.ops) {
        if (op.kind == 0) {
            if (op.fn) {
                op.fn();
            }
            continue;
        }
        const ListBatch &b = L.batches[static_cast<size_t>(op.batch)];
        bool done = false;
        if (L.cacheable && L.replays >= 2 && g_sh.dev && !b.v.empty()) {
            flush_pending(c);
            int shade = 0, nu = 0;
            bool white = false;
            int units[2] = {0, 0};
            if (sync_device_state(c, &shade, &white, units, &nu) && can_use_buffer(c, L, shade, white, units, nu) &&
                ensure_buffer(L, c)) {
                uint8_t cc[4] = {255, 255, 255, 255};
                const bool cconst = !b.has_color;
                if (cconst) {
                    const State &s = c->st;
                    const bool lit_const = s.lighting && !(s.color_material && (s.color_material_mode == GL_DIFFUSE ||
                                                                                 s.color_material_mode == GL_AMBIENT_AND_DIFFUSE));
                    const float *src = white ? nullptr : (lit_const ? s.material[0].diffuse : s.cur_color);
                    for (int k = 0; k < 4; k++) cc[k] = src ? to8(src[k]) : 255;
                }
                send_layout_and_matrix(c, L.layout, L.origin, cconst, cc);
                espgpu_cmd_draw_buffer(g_sh.dev, dev_prim(b.mode), L.buffer_id, b.base_vertex,
                                       static_cast<uint32_t>(b.v.size()), 0, 0);
                done = true;
            }
        }
        if (!done) {
            replay_batch_inline(c, b);
        }
        after_batch(c, b);
    }

    c->rec = std::move(saved);
    c->replay_depth = depth - 1 < 0 ? 0 : depth - 1;
}

void free_list(DisplayList &L)
{
    if (L.buffer_id) {
        if (g_sh.dev && L.buffer_valid) {
            espgpu_buffer_delete(g_sh.dev, L.buffer_id);
        }
        g_sh.buffer_to_list.erase(L.buffer_id);
    }
}

} // namespace

bool list_recording(Context *c)
{
    return c->rec != nullptr;
}

bool list_compile_only(Context *c)
{
    return c->rec != nullptr && c->rec->mode == GL_COMPILE;
}

void list_record_fn(Context *c, std::function<void()> fn)
{
    if (!c->rec) {
        return;
    }
    ListOp op;
    op.kind = 0;
    op.fn = std::move(fn);
    c->rec->list->ops.push_back(std::move(op));
}

void list_push_batch(Context *c, ListBatch &&b)
{
    if (!c->rec || b.v.empty()) {
        return;
    }
    DisplayList &L = *c->rec->list;
    ListOp op;
    op.kind = 1;
    op.batch = static_cast<int>(L.batches.size());
    L.batches.push_back(std::move(b));
    L.ops.push_back(std::move(op));
}

void list_on_evicted(uint32_t buffer_id)
{
    auto it = g_sh.buffer_to_list.find(buffer_id);
    if (it == g_sh.buffer_to_list.end()) {
        return;
    }
    auto li = g_sh.lists.find(it->second);
    if (li != g_sh.lists.end()) {
        li->second->buffer_valid = false;
    }
}

void impl_glNewList(GLuint list, GLenum mode)
{
    ESPGL_CTX(c);
    if (list == 0 || (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE)) {
        set_error(list == 0 ? GL_INVALID_VALUE : GL_INVALID_ENUM);
        return;
    }
    if (c->rec || c->in_begin) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto r = std::make_unique<Recorder>();
    r->name = list;
    r->mode = mode;
    r->list = std::make_unique<DisplayList>();
    c->rec = std::move(r);
    recorder_reset_attrs(c);
}

void impl_glEndList(void)
{
    ESPGL_CTX(c);
    if (!c->rec) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    std::unique_ptr<Recorder> r = std::move(c->rec);
    if (r->in_begin) {
        /* glBegin 没有配对 glEnd：丢弃半截批次 */
        r->in_begin = false;
    }
    analyse(*r->list);
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    auto it = g_sh.lists.find(r->name);
    if (it != g_sh.lists.end()) {
        free_list(*it->second);
    }
    DisplayList *L = r->list.get();
    g_sh.lists[r->name] = std::move(r->list);
    if (L->cacheable) {
        L->buffer_id = g_sh.next_buffer_id++;
        g_sh.buffer_to_list[L->buffer_id] = r->name;
    }
}

void impl_glCallList(GLuint list)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glCallList(list));
    execute(c, list, c->replay_depth + 1);
}

void impl_glCallLists(GLsizei n, GLenum type, const GLvoid *lists)
{
    ESPGL_CTX(c);
    if (n <= 0 || lists == nullptr) {
        return;
    }
    std::vector<GLuint> names(static_cast<size_t>(n));
    const auto *p = static_cast<const uint8_t *>(lists);
    for (GLsizei i = 0; i < n; i++) {
        GLuint v = 0;
        switch (type) {
        case GL_BYTE: v = static_cast<GLuint>(static_cast<int8_t>(p[i])); break;
        case GL_UNSIGNED_BYTE: v = p[i]; break;
        case GL_SHORT: { int16_t t; memcpy(&t, p + i * 2, 2); v = static_cast<GLuint>(t); break; }
        case GL_UNSIGNED_SHORT: { uint16_t t; memcpy(&t, p + i * 2, 2); v = t; break; }
        case GL_INT: { int32_t t; memcpy(&t, p + i * 4, 4); v = static_cast<GLuint>(t); break; }
        case GL_UNSIGNED_INT: memcpy(&v, p + i * 4, 4); break;
        case GL_FLOAT: { float t; memcpy(&t, p + i * 4, 4); v = static_cast<GLuint>(t); break; }
        case GL_2_BYTES: v = static_cast<GLuint>(p[i * 2] << 8 | p[i * 2 + 1]); break;
        case GL_3_BYTES: v = static_cast<GLuint>(p[i * 3] << 16 | p[i * 3 + 1] << 8 | p[i * 3 + 2]); break;
        case GL_4_BYTES:
            v = static_cast<GLuint>(p[i * 4]) << 24 | static_cast<GLuint>(p[i * 4 + 1]) << 16 |
                static_cast<GLuint>(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
            break;
        default: set_error(GL_INVALID_ENUM); return;
        }
        names[static_cast<size_t>(i)] = v;
    }
    const GLuint base = c->st.list_base;
    if (list_recording(c)) {
        list_record_fn(c, [names, base]() {
            Context *cc = cur();
            if (cc) {
                for (GLuint nm : names) execute(cc, base + nm, cc->replay_depth + 1);
            }
        });
        if (list_compile_only(c)) {
            return;
        }
    }
    for (GLuint nm : names) {
        execute(c, base + nm, c->replay_depth + 1);
    }
}

GLuint impl_glGenLists(GLsizei range)
{
    if (range <= 0) {
        set_error(GL_INVALID_VALUE);
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    GLuint start = 1;
    for (;;) {
        bool free = true;
        for (GLsizei i = 0; i < range; i++) {
            if (g_sh.lists.count(start + static_cast<GLuint>(i))) {
                free = false;
                start += static_cast<GLuint>(i) + 1;
                break;
            }
        }
        if (free) {
            break;
        }
    }
    /* 占位，使 glIsList 返回真、后续 GenLists 不重复分配 */
    for (GLsizei i = 0; i < range; i++) {
        g_sh.lists[start + static_cast<GLuint>(i)] = std::make_unique<DisplayList>();
    }
    return start;
}

void impl_glDeleteLists(GLuint list, GLsizei range)
{
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    for (GLsizei i = 0; i < range; i++) {
        auto it = g_sh.lists.find(list + static_cast<GLuint>(i));
        if (it != g_sh.lists.end()) {
            free_list(*it->second);
            g_sh.lists.erase(it);
        }
    }
}

GLboolean impl_glIsList(GLuint list)
{
    return g_sh.lists.count(list) ? 1 : 0;
}

void impl_glListBase(GLuint base)
{
    ESPGL_CTX(c);
    ESPGL_RECORD(c, impl_glListBase(base));
    c->st.list_base = base;
}

} // namespace espgl
