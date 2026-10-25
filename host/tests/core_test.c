/*
 * core_test.c — 设备核心的主机端冒烟测试（不需要硬件）
 *
 * 直接把命令流喂给 firmware/core，把 PRESENT 出来的帧写成 BMP：
 *   core_2d.bmp   M2 的 2D 路径（棋盘背景 + alpha 精灵 + 进度条）
 *   core_3d.bmp   透视立方体（深度 / 高洛德 / 背面剔除）+ 透视校正地板纹理 + 雾 + 线
 *   core_vm.bmp   片元 VM 程序（等离子）+ FBO 渲染到纹理再贴回屏幕
 * 并检查若干像素的期望值。返回非 0 表示失败。
 *
 * 之后是一组回归测试（覆盖规则、透视跨度、状态快照、VM、调试视图、坏输入）。
 *
 *   cc -std=c11 -Ishared -Ifirmware/core host/tests/core_test.c firmware/core/[a-z]*.c -lm
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "espgpu_proto.h"
#include "espgpu_vm.h"

static uint8_t g_buf[256 * 1024];
static uint32_t g_len;
static uint16_t g_frame[800 * 600];
static int g_w, g_h, g_frames;
static int g_fails;

static void present(void *user, const uint16_t *px, uint16_t w, uint16_t h)
{
    (void)user;
    memcpy(g_frame, px, (size_t)w * h * 2);
    g_w = w;
    g_h = h;
    g_frames++;
}

static void push(uint8_t op, uint8_t flags, const void *body, uint32_t size)
{
    espgpu_cmd_t c = {op, flags, (uint16_t)size};
    memcpy(g_buf + g_len, &c, 4);
    g_len += 4;
    if (size) {
        memcpy(g_buf + g_len, body, size);
        g_len += size;
    }
}

static void submit(gpu_core_t *c)
{
    gpu_core_exec(c, g_buf, g_len);
    g_len = 0;
}

static void write_bmp(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int row = g_w * 3, pad = (4 - row % 4) % 4;
    const uint32_t size = 54 + (uint32_t)(row + pad) * g_h;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &g_w, 4);
    memcpy(hdr + 22, &g_h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (int y = g_h - 1; y >= 0; y--) {
        for (int x = 0; x < g_w; x++) {
            const uint16_t v = g_frame[y * g_w + x];
            const uint8_t bgr[3] = {(uint8_t)((v & 31) << 3), (uint8_t)(((v >> 5) & 63) << 2),
                                    (uint8_t)((v >> 11) << 3)};
            fwrite(bgr, 1, 3, f);
        }
        static const uint8_t z[3];
        fwrite(z, 1, pad, f);
    }
    fclose(f);
}

static uint16_t rgb(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void expect(const char *what, int x, int y, int r, int g, int b, int tol)
{
    const uint16_t v = g_frame[y * g_w + x];
    const int pr = (v >> 11) << 3, pg = ((v >> 5) & 63) << 2, pb = (v & 31) << 3;
    if (abs(pr - r) > tol || abs(pg - g) > tol || abs(pb - b) > tol) {
        printf("FAIL %s @(%d,%d): got %d,%d,%d want %d,%d,%d\n", what, x, y, pr, pg, pb, r, g, b);
        g_fails++;
    }
}

static void upload_tex(uint32_t id, int w, int h, uint8_t fmt, const void *px)
{
    const unsigned bpp = (fmt == ESPGPU_FMT_INDEX8 || fmt == ESPGPU_FMT_R8) ? 1 : 2;
    uint8_t body[sizeof(espgpu_upload_texture_t) + 64 * 64 * 2];
    espgpu_upload_texture_t u;
    memset(&u, 0, sizeof(u));
    u.id = id;
    u.width = (uint16_t)w;
    u.height = (uint16_t)h;
    u.total_len = (uint32_t)(w * h * bpp);
    u.format = fmt;
    memcpy(body, &u, sizeof(u));
    memcpy(body + sizeof(u), px, u.total_len);
    push(ESPGPU_OP_UPLOAD_TEXTURE, 0, body, (uint32_t)sizeof(u) + u.total_len);
}

static void bind(uint16_t unit, uint16_t id, uint8_t filter, uint8_t wrap)
{
    espgpu_bind_texture_t b = {unit, id, filter, wrap, 0};
    push(ESPGPU_OP_BIND_TEXTURE, 0, &b, sizeof(b));
}

static void mat(uint8_t slot, const float m[16])
{
    uint8_t body[4 + 64];
    espgpu_set_matrix_t h = {slot, ESPGPU_MATF_FLOAT, 0};
    memcpy(body, &h, 4);
    memcpy(body + 4, m, 64);
    push(ESPGPU_OP_SET_MATRIX, 0, body, sizeof(body));
}

static void mat_mul(const float *a, const float *b, float *o)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
}

static void perspective(float fovy, float aspect, float n, float f, float *m)
{
    const float t = 1.0f / tanf(fovy * 0.5f);
    memset(m, 0, 64);
    m[0] = t / aspect;
    m[5] = t;
    m[10] = (f + n) / (n - f);
    m[11] = 2 * f * n / (n - f);
    m[14] = -1;
}

typedef struct {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
} vtx_t;

static void layout_xyz_uv_rgba(void)
{
    espgpu_vertex_layout_t l;
    memset(&l, 0, sizeof(l));
    l.stride = sizeof(vtx_t);
    l.attrib_count = 3;
    l.attribs[0] = (espgpu_vertex_attrib_t){ESPGPU_ATTR_POSITION, ESPGPU_TYPE_F32, 3, 0, 0, 0, 0};
    l.attribs[1] = (espgpu_vertex_attrib_t){ESPGPU_ATTR_TEXCOORD0, ESPGPU_TYPE_F32, 2, 0, 12, 0, 0};
    l.attribs[2] = (espgpu_vertex_attrib_t){ESPGPU_ATTR_COLOR, ESPGPU_TYPE_U8, 4, 1, 20, 0, 0};
    push(ESPGPU_OP_VERTEX_LAYOUT, 0, &l, sizeof(l));
}

static void draw(uint8_t prim, const vtx_t *v, int n, const uint16_t *idx, int ni)
{
    uint8_t body[sizeof(espgpu_draw_t) + 64 * sizeof(vtx_t) + 128];
    espgpu_draw_t d;
    memset(&d, 0, sizeof(d));
    d.prim = prim;
    d.vertex_count = (uint16_t)n;
    d.index_count = (uint16_t)ni;
    memcpy(body, &d, sizeof(d));
    memcpy(body + sizeof(d), v, (size_t)n * sizeof(vtx_t));
    if (ni) memcpy(body + sizeof(d) + n * sizeof(vtx_t), idx, (size_t)ni * 2);
    push(ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, body,
         (uint32_t)(sizeof(d) + n * sizeof(vtx_t) + ni * 2));
}

static void pipeline(uint8_t shade, int depth, int cull, int blend, int fog)
{
    uint8_t body[sizeof(espgpu_pipeline_t) + sizeof(espgpu_pipeline_fog_t) +
                 sizeof(espgpu_pipeline_ext_t)];
    espgpu_pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.mask = 0xFFFF & ~ESPGPU_PS_TEXTURE;
    p.blend_enable = (uint8_t)blend;
    p.blend_src = ESPGPU_BLEND_SRC_ALPHA;
    p.blend_dst = ESPGPU_BLEND_ONE_MINUS_SRC_ALPHA;
    p.depth_test = (uint8_t)depth;
    p.depth_write = (uint8_t)depth;
    p.depth_func = ESPGPU_CMP_LEQUAL;
    p.alpha_func = ESPGPU_CMP_ALWAYS;
    p.cull_mode = cull ? ESPGPU_CULL_BACK : ESPGPU_CULL_NONE;
    p.front_face = ESPGPU_FRONT_CCW;
    p.shade_mode = shade;
    p.fog_enable = (uint8_t)fog;
    p.fog_mode = ESPGPU_FOG_LINEAR;
    p.fog_color = rgb(40, 40, 60);
    espgpu_pipeline_fog_t f = {2.0f, 12.0f, 1.0f};
    espgpu_pipeline_ext_t e;
    memset(&e, 0, sizeof(e));
    e.shade_model = ESPGPU_SHADE_MODEL_SMOOTH;
    e.color_write = 1;
    memcpy(body, &p, sizeof(p));
    memcpy(body + sizeof(p), &f, sizeof(f));
    memcpy(body + sizeof(p) + sizeof(f), &e, sizeof(e));
    push(ESPGPU_OP_PIPELINE, 0, body, sizeof(body));
}

static void clear(uint16_t color)
{
    espgpu_clear_t c = {3, 0, color, 0xFFFF};
    push(ESPGPU_OP_CLEAR, 0, &c, sizeof(c));
}

/* ── 场景 ─────────────────────────────────────────────────────────── */

static void scene_2d(gpu_core_t *c)
{
    static uint16_t checker[32 * 32];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            checker[y * 32 + x] = ((x / 8 + y / 8) & 1) ? rgb(200, 200, 200) : rgb(60, 60, 90);
    static uint16_t sprite[16 * 16];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            const int a = (x > 3 && x < 12 && y > 3 && y < 12) ? 15 : 0;
            sprite[y * 16 + x] = (uint16_t)((15 << 12) | (8 << 8) | (0 << 4) | a);
        }
    upload_tex(1, 32, 32, ESPGPU_FMT_RGB565, checker);
    upload_tex(2, 16, 16, ESPGPU_FMT_RGBA4444, sprite);

    espgpu_clear_t cl = {1, 0, rgb(0, 0, 0), 0};
    push(ESPGPU_OP_CLEAR, 0, &cl, sizeof(cl));
    espgpu_pipeline2d_t p = {ESPGPU_BLEND_NONE, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT, 0, 0, {0}};
    push(ESPGPU_OP_PIPELINE2D, 0, &p, sizeof(p));
    bind(0, 1, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT);
    /* 全屏背景，纹理重复 10x6 次 */
    espgpu_vertex2d_t q[4] = {
        {0, 0, 0, 0, 0xFFFF},
        {320 * 16, 0, 320 * 16, 0, 0xFFFF},
        {320 * 16, 200 * 16, 320 * 16, 200 * 16, 0xFFFF},
        {0, 200 * 16, 0, 200 * 16, 0xFFFF},
    };
    uint8_t body[sizeof(espgpu_draw_t) + sizeof(q)];
    espgpu_draw_t d;
    memset(&d, 0, sizeof(d));
    d.prim = ESPGPU_PRIM_QUADS;
    d.vertex_count = 4;
    memcpy(body, &d, sizeof(d));
    memcpy(body + sizeof(d), q, sizeof(q));
    push(ESPGPU_OP_DRAW, 0, body, sizeof(body));

    /* alpha 精灵 */
    espgpu_pipeline2d_t pa = {ESPGPU_BLEND_ALPHA, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_CLAMP, 1, 128, {0}};
    push(ESPGPU_OP_PIPELINE2D, 0, &pa, sizeof(pa));
    bind(0, 2, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_CLAMP);
    espgpu_vertex2d_t s[4] = {
        {100 * 16, 50 * 16, 0, 0, 0xFFFF},
        {164 * 16, 50 * 16, 16 * 16, 0, 0xFFFF},
        {164 * 16, 114 * 16, 16 * 16, 16 * 16, 0xFFFF},
        {100 * 16, 114 * 16, 0, 16 * 16, 0xFFFF},
    };
    memcpy(body + sizeof(d), s, sizeof(s));
    push(ESPGPU_OP_DRAW, 0, body, sizeof(body));

    /* 纯色进度条（无纹理） */
    bind(0, 0, 0, 0);
    espgpu_pipeline2d_t pn = {ESPGPU_BLEND_NONE, 0, 0, 0, 0, {0}};
    push(ESPGPU_OP_PIPELINE2D, 0, &pn, sizeof(pn));
    espgpu_vertex2d_t b[4] = {
        {20 * 16, 180 * 16, 0, 0, rgb(0, 255, 0)},
        {220 * 16, 180 * 16, 0, 0, rgb(0, 255, 0)},
        {220 * 16, 190 * 16, 0, 0, rgb(0, 255, 0)},
        {20 * 16, 190 * 16, 0, 0, rgb(0, 255, 0)},
    };
    memcpy(body + sizeof(d), b, sizeof(b));
    push(ESPGPU_OP_DRAW, 0, body, sizeof(body));
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    write_bmp("core_2d.bmp");

    expect("2d bar", 100, 185, 0, 255, 0, 8);
    expect("2d sprite", 132, 82, 255, 136, 0, 12);
    expect("2d checker dark", 2, 2, 60, 60, 90, 8);
    expect("2d checker light", 10, 2, 200, 200, 200, 8);
}

static void scene_3d(gpu_core_t *c, const char *out, bool check)
{
    static uint16_t floor_tex[64 * 64];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            floor_tex[y * 64 + x] = ((x / 8 + y / 8) & 1) ? rgb(220, 180, 120) : rgb(80, 50, 30);
    upload_tex(10, 64, 64, ESPGPU_FMT_RGB565, floor_tex);

    clear(rgb(40, 40, 60));
    float proj[16], view[16], mvp[16];
    perspective(1.0f, 320.0f / 200.0f, 0.5f, 50.0f, proj);
    memset(view, 0, 64);
    view[0] = view[5] = view[10] = view[15] = 1;
    view[7] = -1.0f;  /* 相机抬高 1 */
    view[11] = -4.0f; /* 往后 4 */
    mat_mul(proj, view, mvp);
    mat(ESPGPU_MATRIX_MVP, mvp);
    mat(ESPGPU_MATRIX_MODELVIEW, view);
    layout_xyz_uv_rgba();

    /* 地板：透视校正 + 雾 */
    pipeline(ESPGPU_SHADE_TEX_MOD_COLOR, 1, 0, 0, 1);
    bind(0, 10, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT);
    vtx_t fl[4] = {
        {-10, 0, 2, 0, 0, 255, 255, 255, 255},
        {10, 0, 2, 20, 0, 255, 255, 255, 255},
        {10, 0, -30, 20, 32, 255, 255, 255, 255},
        {-10, 0, -30, 0, 32, 255, 255, 255, 255},
    };
    draw(ESPGPU_PRIM_QUADS, fl, 4, NULL, 0);

    /* 旋转立方体：顶点色 + 背面剔除 + 深度 */
    float model[16], mv[16], m2[16];
    const float a = 0.6f, b2 = 0.4f;
    float ry[16] = {cosf(a), 0, sinf(a), 0, 0, 1, 0, 0, -sinf(a), 0, cosf(a), 0, 0, 0, 0, 1};
    float rx[16] = {1, 0, 0, 0, 0, cosf(b2), -sinf(b2), 0, 0, sinf(b2), cosf(b2), 0, 0, 0, 0, 1};
    mat_mul(ry, rx, model);
    model[7] = 1.0f;
    model[11] = 0.0f;
    mat_mul(view, model, mv);
    mat_mul(proj, mv, m2);
    mat(ESPGPU_MATRIX_MVP, m2);
    pipeline(ESPGPU_SHADE_FLAT, 1, 1, 0, 0);
    bind(0, 0, 0, 0);
    vtx_t cube[8];
    for (int i = 0; i < 8; i++) {
        cube[i].x = (i & 1) ? 0.7f : -0.7f;
        cube[i].y = (i & 2) ? 0.7f : -0.7f;
        cube[i].z = (i & 4) ? 0.7f : -0.7f;
        cube[i].u = cube[i].v = 0;
        cube[i].r = (i & 1) ? 255 : 30;
        cube[i].g = (i & 2) ? 255 : 30;
        cube[i].b = (i & 4) ? 255 : 30;
        cube[i].a = 255;
    }
    /* 逆时针（从外看）的 12 个三角形 */
    static const uint16_t idx[36] = {
        0, 2, 3, 0, 3, 1, /* -z */ 4, 5, 7, 4, 7, 6, /* +z */
        0, 4, 6, 0, 6, 2, /* -x */ 1, 3, 7, 1, 7, 5, /* +x */
        0, 1, 5, 0, 5, 4, /* -y */ 2, 6, 7, 2, 7, 3, /* +y */
    };
    draw(ESPGPU_PRIM_TRIANGLES, cube, 8, idx, 36);

    /* 线 */
    mat(ESPGPU_MATRIX_MVP, mvp);
    pipeline(ESPGPU_SHADE_FLAT, 0, 0, 0, 0);
    vtx_t ln[2] = {{-3, 0.01f, 0, 0, 0, 255, 0, 0, 255}, {3, 0.01f, 0, 0, 0, 255, 0, 0, 255}};
    draw(ESPGPU_PRIM_LINES, ln, 2, NULL, 0);

    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    write_bmp(out);
    if (check) {
        expect("3d sky", 5, 5, 40, 40, 60, 8);
        /* 立方体正中应被画到（非背景色） */
        const uint16_t mid = g_frame[100 * g_w + 160];
        if (mid == rgb(40, 40, 60)) {
            printf("FAIL cube missing at center\n");
            g_fails++;
        }
    }
}

static espgpu_vm_insn_t I(uint8_t op, uint8_t dst, uint8_t mask, uint8_t s0, uint8_t s1,
                          uint8_t s2, uint8_t sw0, uint8_t sw1, uint8_t sw2)
{
    espgpu_vm_insn_t in;
    memset(&in, 0, sizeof(in));
    in.op = op;
    in.dst = dst;
    in.mask = mask;
    in.src[0] = s0;
    in.src[1] = s1;
    in.src[2] = s2;
    in.swz[0] = sw0;
    in.swz[1] = sw1;
    in.swz[2] = sw2;
    return in;
}

static void scene_vm(gpu_core_t *c)
{
    /* FBO：渲染一张 64x64 的蓝底红三角到纹理 20 */
    espgpu_fbo_t f;
    memset(&f, 0, sizeof(f));
    f.action = ESPGPU_FBO_BIND_TEXTURE;
    f.texture_id = 20;
    f.width = 64;
    f.height = 64;
    push(ESPGPU_OP_FBO, 0, &f, sizeof(f));
    mat(ESPGPU_MATRIX_MVP, (const float[16]){1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    clear(rgb(0, 0, 255));
    pipeline(ESPGPU_SHADE_FLAT, 0, 0, 0, 0);
    layout_xyz_uv_rgba();
    /* 只覆盖上半部分（NDC y > 0）：FBO 里 NDC y=-1 是第 0 行 */
    vtx_t tri[3] = {{-1, 0, 0, 0, 0, 255, 0, 0, 255},
                    {1, 0, 0, 0, 0, 255, 0, 0, 255},
                    {0, 1, 0, 0, 0, 255, 0, 0, 255}};
    draw(ESPGPU_PRIM_TRIANGLES, tri, 3, NULL, 0);
    f.action = ESPGPU_FBO_BIND_DEFAULT;
    push(ESPGPU_OP_FBO, 0, &f, sizeof(f));

    /* 片元程序：等离子 */
    struct {
        espgpu_load_program_t h;
        espgpu_vm_insn_t code[8];
    } prog;
    memset(&prog, 0, sizeof(prog));
    const uint8_t T0 = ESPGPU_VM_REG(ESPGPU_VM_BANK_TEMP, 0);
    const uint8_t V6 = ESPGPU_VM_REG(ESPGPU_VM_BANK_VARYING, 6);
    const uint8_t C0 = ESPGPU_VM_REG(ESPGPU_VM_BANK_CONST, 0);
    const uint8_t ID = ESPGPU_VM_SWZ_IDENTITY;
    prog.code[0] = I(ESPGPU_VM_MUL, T0, 0x3, V6, C0, 0, ID, ID, 0);                /* t.xy = frag*k */
    prog.code[1] = I(ESPGPU_VM_SIN, T0, 0x3, T0, 0, 0, ID, 0, 0);                  /* sin */
    prog.code[2] = I(ESPGPU_VM_MAD, T0, 0x3, T0, C0, C0, ID, ESPGPU_VM_SWZ(2, 2, 2, 2),
                     ESPGPU_VM_SWZ(2, 2, 2, 2));                                    /* *0.5+0.5 */
    prog.code[3] = I(ESPGPU_VM_MOV, ESPGPU_VM_OUT, 0x3, T0, 0, 0, ID, 0, 0);
    prog.code[4] = I(ESPGPU_VM_MOV, ESPGPU_VM_OUT, 0x4, C0, 0, 0, ESPGPU_VM_SWZ(3, 3, 3, 3), 0, 0);
    prog.code[5] = I(ESPGPU_VM_MOV, ESPGPU_VM_OUT, 0x8, ESPGPU_VM_ONE, 0, 0, ID, 0, 0);
    prog.code[6] = I(ESPGPU_VM_END, 0, 0, 0, 0, 0, 0, 0, 0);
    prog.h.program_id = 3;
    prog.h.varying_count = 1;
    prog.h.insn_count = 7;
    push(ESPGPU_OP_LOAD_PROGRAM, 0, &prog, sizeof(espgpu_load_program_t) + 7 * 12);
    struct {
        espgpu_set_uniforms_t h;
        float v[4];
    } un = {{0, 1, 0}, {0.1f, 0.07f, 0.5f, 0.25f}};
    push(ESPGPU_OP_SET_UNIFORMS, 0, &un, sizeof(un));

    clear(rgb(0, 0, 0));
    uint8_t body[sizeof(espgpu_pipeline_t) + sizeof(espgpu_pipeline_fog_t) +
                 sizeof(espgpu_pipeline_ext_t)];
    espgpu_pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.mask = ESPGPU_PS_SHADE | ESPGPU_PS_DEPTH | ESPGPU_PS_BLEND;
    p.shade_mode = ESPGPU_SHADE_PROGRAM;
    espgpu_pipeline_fog_t fg = {0, 1, 1};
    espgpu_pipeline_ext_t e;
    memset(&e, 0, sizeof(e));
    e.color_write = 1;
    e.program_id = 3;
    e.shade_model = 1;
    memcpy(body, &p, sizeof(p));
    memcpy(body + sizeof(p), &fg, sizeof(fg));
    memcpy(body + sizeof(p) + sizeof(fg), &e, sizeof(e));
    push(ESPGPU_OP_PIPELINE, 0, body, sizeof(body));
    vtx_t full[4] = {{-1, -1, 0, 0, 0, 255, 255, 255, 255},
                     {1, -1, 0, 1, 0, 255, 255, 255, 255},
                     {1, 1, 0, 1, 1, 255, 255, 255, 255},
                     {-1, 1, 0, 0, 1, 255, 255, 255, 255}};
    draw(ESPGPU_PRIM_QUADS, full, 4, NULL, 0);

    /* 把 FBO 纹理贴到右上角：纹理 t=1 在上，应看到红三角尖朝上 */
    pipeline(ESPGPU_SHADE_TEX, 0, 0, 0, 0);
    bind(0, 20, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_CLAMP);
    vtx_t q[4] = {{0.4f, 0.2f, 0, 0, 0, 255, 255, 255, 255},
                  {0.95f, 0.2f, 0, 1, 0, 255, 255, 255, 255},
                  {0.95f, 0.95f, 0, 1, 1, 255, 255, 255, 255},
                  {0.4f, 0.95f, 0, 0, 1, 255, 255, 255, 255}};
    draw(ESPGPU_PRIM_QUADS, q, 4, NULL, 0);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    write_bmp("core_vm.bmp");

    /* 贴图区域：下半应为蓝，上半中间应为红 */
    const int qx0 = (int)((0.4f + 1) * 0.5f * 320), qx1 = (int)((0.95f + 1) * 0.5f * 320);
    const int qy_top = (int)((1 - 0.95f) * 0.5f * 200), qy_bot = (int)((1 - 0.2f) * 0.5f * 200);
    expect("fbo bottom (blue)", (qx0 + qx1) / 2, qy_bot - 3, 0, 0, 255, 8);
    expect("fbo top (red)", (qx0 + qx1) / 2, qy_top + (qy_bot - qy_top) / 3, 255, 0, 0, 8);
}


/* ── 回归测试 ─────────────────────────────────────────────────────── */

static void pipeline_ex(uint8_t shade, int dtest, int dwrite, int blend, uint8_t bsrc, uint8_t bdst)
{
    uint8_t body[sizeof(espgpu_pipeline_t) + sizeof(espgpu_pipeline_fog_t) +
                 sizeof(espgpu_pipeline_ext_t)];
    espgpu_pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.mask = 0xFFFF & ~ESPGPU_PS_TEXTURE;
    p.blend_enable = (uint8_t)blend;
    p.blend_src = bsrc;
    p.blend_dst = bdst;
    p.depth_test = (uint8_t)dtest;
    p.depth_write = (uint8_t)dwrite;
    p.depth_func = ESPGPU_CMP_LEQUAL;
    p.alpha_func = ESPGPU_CMP_ALWAYS;
    p.shade_mode = shade;
    espgpu_pipeline_fog_t f = {0, 1, 1};
    espgpu_pipeline_ext_t e;
    memset(&e, 0, sizeof(e));
    e.shade_model = ESPGPU_SHADE_MODEL_SMOOTH;
    e.color_write = 1;
    memcpy(body, &p, sizeof(p));
    memcpy(body + sizeof(p), &f, sizeof(f));
    memcpy(body + sizeof(p) + sizeof(f), &e, sizeof(e));
    push(ESPGPU_OP_PIPELINE, 0, body, sizeof(body));
}

static const float k_ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

/** 只有 POSITION（n 分量 F32）的布局 */
static void layout_pos(int comps)
{
    espgpu_vertex_layout_t l;
    memset(&l, 0, sizeof(l));
    l.stride = (uint16_t)(comps * 4);
    l.attrib_count = 1;
    l.attribs[0] = (espgpu_vertex_attrib_t){ESPGPU_ATTR_POSITION, ESPGPU_TYPE_F32, (uint8_t)comps, 0, 0, 0, 0};
    push(ESPGPU_OP_VERTEX_LAYOUT, 0, &l, sizeof(l));
}

static void draw_raw(uint8_t prim, const void *v, int n, int stride)
{
    static uint8_t body[sizeof(espgpu_draw_t) + 4096 * 16];
    espgpu_draw_t d;
    memset(&d, 0, sizeof(d));
    d.prim = prim;
    d.vertex_count = (uint16_t)n;
    memcpy(body, &d, sizeof(d));
    memcpy(body + sizeof(d), v, (size_t)n * stride);
    push(ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, body, (uint32_t)(sizeof(d) + (size_t)n * stride));
}

static void set_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    espgpu_color_t col = {r, g, b, a};
    push(ESPGPU_OP_COLOR, 0, &col, sizeof(col));
}

static uint32_t g_rs = 12345;
static float frand(float a, float b)
{
    g_rs = g_rs * 1103515245u + 12345u;
    return a + (b - a) * (float)((g_rs >> 8) & 0xFFFF) / 65535.0f;
}

/* top-left 规则：共享边的网格（含跨 tile、出屏、亚像素抖动）每个像素恰好被画一次 */
static void test_coverage(gpu_core_t *c)
{
    enum { GX = 11, GY = 9 };
    static float vx[GY][GX], vy[GY][GX];
    for (int j = 0; j < GY; j++)
        for (int i = 0; i < GX; i++) {
            vx[j][i] = -1.3f + 2.6f * (float)i / (GX - 1);
            vy[j][i] = -1.3f + 2.6f * (float)j / (GY - 1);
            if (i > 0 && i < GX - 1) vx[j][i] += frand(-0.08f, 0.08f);
            if (j > 0 && j < GY - 1) vy[j][i] += frand(-0.08f, 0.08f);
        }
    static float tri[(GX - 1) * (GY - 1) * 6 * 2 + 64 * 3 * 2];
    int n = 0;
    for (int j = 0; j + 1 < GY; j++)
        for (int i = 0; i + 1 < GX; i++) {
            const float ax = vx[j][i], ay = vy[j][i], bx = vx[j][i + 1], by = vy[j][i + 1];
            const float cx = vx[j + 1][i + 1], cy = vy[j + 1][i + 1], dx = vx[j + 1][i], dy = vy[j + 1][i];
            const float q[6][2] = {{ax, ay}, {bx, by}, {cx, cy}, {ax, ay}, {cx, cy}, {dx, dy}};
            const float r[6][2] = {{ax, ay}, {bx, by}, {dx, dy}, {bx, by}, {cx, cy}, {dx, dy}};
            const float(*use)[2] = ((i + j) & 1) ? q : r;
            for (int k = 0; k < 6; k++) {
                tri[n * 2] = use[k][0];
                tri[n * 2 + 1] = use[k][1];
                n++;
            }
        }
    clear(rgb(0, 0, 0));
    mat(ESPGPU_MATRIX_MVP, k_ident);
    layout_pos(2);
    pipeline_ex(ESPGPU_SHADE_FLAT, 0, 0, 1, ESPGPU_BLEND_ONE, ESPGPU_BLEND_ONE);
    set_color(8, 0, 0, 255);
    draw_raw(ESPGPU_PRIM_TRIANGLES, tri, n, 8);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    int bad = 0;
    for (int i = 0; i < g_w * g_h; i++) bad += g_frame[i] != 0x0800;
    if (bad) {
        printf("FAIL coverage: %d pixels not hit exactly once\n", bad);
        g_fails++;
    }

    /* 扇形：64 个细长三角形共享圆心，不能有洞或重复 */
    n = 0;
    const float cx = 0.1f, cy = -0.05f;
    for (int k = 0; k < 64; k++) {
        const float a0 = 6.2831853f * k / 64, a1 = 6.2831853f * (k + 1) / 64;
        const float p[3][2] = {{cx, cy}, {cx + 0.9f * cosf(a0), cy + 0.9f * sinf(a0) * 1.6f},
                               {cx + 0.9f * cosf(a1), cy + 0.9f * sinf(a1) * 1.6f}};
        for (int m = 0; m < 3; m++) {
            tri[n * 2] = p[m][0];
            tri[n * 2 + 1] = p[m][1];
            n++;
        }
    }
    clear(rgb(0, 0, 0));
    draw_raw(ESPGPU_PRIM_TRIANGLES, tri, n, 8);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    bad = 0;
    for (int y = 0; y < g_h; y++)
        for (int x = 0; x < g_w; x++) {
            const uint16_t v = g_frame[y * g_w + x];
            if (v != 0 && v != 0x0800) bad++;
            /* 圆心附近必须被画到 */
            const float nx = (x + 0.5f) / g_w * 2 - 1 - cx, ny = 1 - (y + 0.5f) / g_h * 2 - cy;
            if (nx * nx + (ny / 1.6f) * (ny / 1.6f) < 0.6f * 0.6f && v != 0x0800) bad++;
        }
    if (bad) {
        printf("FAIL fan coverage: %d bad pixels\n", bad);
        g_fails++;
    }
}

/* 透视：跨度最后一段不能用三角形外的 R+1 外推 1/w（这里那一点 1/w < 0，旧代码整段纹理坐标乱掉） */
static void test_persp_span_end(gpu_core_t *c)
{
    static uint16_t ramp[256];
    for (int i = 0; i < 256; i++) ramp[i] = (uint16_t)i; /* 像素值 = texel 下标 */
    upload_tex(40, 256, 1, ESPGPU_FMT_RGB565, ramp);
    const float w0 = 0.05f, w1 = 50.0f;
    const float xr = -1.0f + 2.0f * 100.0f / 320.0f; /* 屏幕 x = 100 */
    /* POSITION 4 分量（裁剪坐标）+ TEXCOORD0 */
    float v[4][6] = {
        {-1.0f * w0, 1.0f * w0, 0, w0, 0, 0},
        {xr * w1, 1.0f * w1, 0, w1, 1, 0},
        {xr * w1, 0.8f * w1, 0, w1, 1, 0},
        {-1.0f * w0, 0.8f * w0, 0, w0, 0, 0},
    };
    espgpu_vertex_layout_t l;
    memset(&l, 0, sizeof(l));
    l.stride = 24;
    l.attrib_count = 2;
    l.attribs[0] = (espgpu_vertex_attrib_t){ESPGPU_ATTR_POSITION, ESPGPU_TYPE_F32, 4, 0, 0, 0, 0};
    l.attribs[1] = (espgpu_vertex_attrib_t){ESPGPU_ATTR_TEXCOORD0, ESPGPU_TYPE_F32, 2, 0, 16, 0, 0};
    clear(rgb(0, 0, 0));
    mat(ESPGPU_MATRIX_MVP, k_ident);
    push(ESPGPU_OP_VERTEX_LAYOUT, 0, &l, sizeof(l));
    pipeline_ex(ESPGPU_SHADE_TEX, 0, 0, 0, 0, 0);
    bind(0, 40, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_CLAMP);
    draw_raw(ESPGPU_PRIM_QUADS, v, 4, 24);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    /* 每 8 像素线性插值本身有误差（这里曲率极大，段起点又随 tile 边界变化），所以只要求：
     * u 单调不减、跨度最后一个像素精确到 ±2 texel。旧代码最后一段整段塌成 texel 0。 */
    int bad = 0;
    for (int y = 3; y < 17; y++) {
        int prev = -1;
        for (int x = 0; x < 100; x++) {
            const float t = (x + 0.5f) / 100.0f;
            const float u = (t * 256.0f / w1) / ((1 - t) / w0 + t / w1);
            const int want = (int)u > 255 ? 255 : (int)u;
            const int got = g_frame[y * g_w + x];
            if (got < prev) bad++;
            if (x == 99 && abs(got - want) > 2) bad++;
            prev = got;
        }
    }
    if (bad) {
        printf("FAIL perspective span end: %d bad pixels\n", bad);
        g_fails++;
    }
}

static void quad2d(int x0, int y0, int x1, int y1, int u1, int v1, uint16_t col)
{
    espgpu_vertex2d_t q[4] = {
        {(int16_t)(x0 * 16), (int16_t)(y0 * 16), 0, 0, col},
        {(int16_t)(x1 * 16), (int16_t)(y0 * 16), (int16_t)(u1 * 16), 0, col},
        {(int16_t)(x1 * 16), (int16_t)(y1 * 16), (int16_t)(u1 * 16), (int16_t)(v1 * 16), col},
        {(int16_t)(x0 * 16), (int16_t)(y1 * 16), 0, (int16_t)(v1 * 16), col},
    };
    uint8_t body[sizeof(espgpu_draw_t) + sizeof(q)];
    espgpu_draw_t d;
    memset(&d, 0, sizeof(d));
    d.prim = ESPGPU_PRIM_QUADS;
    d.vertex_count = 4;
    memcpy(body, &d, sizeof(d));
    memcpy(body + sizeof(d), q, sizeof(q));
    push(ESPGPU_OP_DRAW, 0, body, sizeof(body));
}

/* PIPELINE 的 PS_TEXTURE 改了单元 0 的绑定，2D 路径缓存的状态快照必须失效 */
static void test_pipeline_rebinds_2d(gpu_core_t *c)
{
    static uint16_t red[16], blue[16];
    for (int i = 0; i < 16; i++) {
        red[i] = rgb(255, 0, 0);
        blue[i] = rgb(0, 0, 255);
    }
    upload_tex(30, 4, 4, ESPGPU_FMT_RGB565, red);
    upload_tex(31, 4, 4, ESPGPU_FMT_RGB565, blue);
    clear(rgb(0, 0, 0));
    espgpu_pipeline2d_t p2 = {ESPGPU_BLEND_NONE, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT, 0, 0, {0}};
    push(ESPGPU_OP_PIPELINE2D, 0, &p2, sizeof(p2));
    bind(0, 30, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT);
    quad2d(0, 0, 100, 100, 4, 4, 0xFFFF);
    espgpu_pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.mask = ESPGPU_PS_TEXTURE;
    p.tex_count = 1;
    p.tex_unit[0] = 0;
    p.tex_id[0] = 31;
    push(ESPGPU_OP_PIPELINE, 0, &p, sizeof(p));
    quad2d(150, 0, 250, 100, 4, 4, 0xFFFF);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    expect("2d before rebind (red)", 50, 50, 255, 0, 0, 8);
    expect("2d after PIPELINE rebind (blue)", 200, 50, 0, 0, 255, 8);
}

/* 片元 VM：临时寄存器每像素清零（r1 += 0.5 → 每个像素都是 0.5，而不是逐像素累加） */
static void test_vm_temps_zeroed(gpu_core_t *c)
{
    struct {
        espgpu_load_program_t h;
        espgpu_vm_insn_t code[3];
    } prog;
    memset(&prog, 0, sizeof(prog));
    const uint8_t R1 = ESPGPU_VM_REG(ESPGPU_VM_BANK_TEMP, 1);
    const uint8_t C0 = ESPGPU_VM_REG(ESPGPU_VM_BANK_CONST, 0);
    const uint8_t ID = ESPGPU_VM_SWZ_IDENTITY;
    prog.code[0] = I(ESPGPU_VM_ADD, R1, 0xF, R1, C0, 0, ID, ID, 0);
    prog.code[1] = I(ESPGPU_VM_MOV, ESPGPU_VM_OUT, 0x7, R1, 0, 0, ID, 0, 0);
    prog.code[2] = I(ESPGPU_VM_END, 0, 0, 0, 0, 0, 0, 0, 0);
    prog.h.program_id = 5;
    prog.h.varying_count = 0;
    prog.h.insn_count = 3;
    push(ESPGPU_OP_LOAD_PROGRAM, 0, &prog, sizeof(espgpu_load_program_t) + 3 * 12);
    struct {
        espgpu_set_uniforms_t h;
        float v[4];
    } un = {{0, 1, 0}, {0.5f, 0.5f, 0.5f, 0.5f}};
    push(ESPGPU_OP_SET_UNIFORMS, 0, &un, sizeof(un));
    clear(rgb(0, 0, 0));
    mat(ESPGPU_MATRIX_MVP, k_ident);
    layout_pos(2);
    uint8_t body[sizeof(espgpu_pipeline_t) + sizeof(espgpu_pipeline_fog_t) +
                 sizeof(espgpu_pipeline_ext_t)];
    espgpu_pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.mask = ESPGPU_PS_SHADE | ESPGPU_PS_DEPTH | ESPGPU_PS_BLEND;
    p.shade_mode = ESPGPU_SHADE_PROGRAM;
    espgpu_pipeline_fog_t fg = {0, 1, 1};
    espgpu_pipeline_ext_t e;
    memset(&e, 0, sizeof(e));
    e.color_write = 1;
    e.program_id = 5;
    e.shade_model = 1;
    memcpy(body, &p, sizeof(p));
    memcpy(body + sizeof(p), &fg, sizeof(fg));
    memcpy(body + sizeof(p) + sizeof(fg), &e, sizeof(e));
    push(ESPGPU_OP_PIPELINE, 0, body, sizeof(body));
    const float full[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    draw_raw(ESPGPU_PRIM_QUADS, full, 4, 8);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    expect("vm temps zeroed per pixel", 160, 100, 128, 128, 128, 8);
}

/* 调试视图：TILES 显示每 tile 三角形数；改分辨率后调试视图仍有效 */
static void test_debug_views(gpu_core_t *c)
{
    mat(ESPGPU_MATRIX_MVP, k_ident);
    layout_pos(2);
    pipeline_ex(ESPGPU_SHADE_FLAT, 0, 0, 0, 0, 0);
    set_color(255, 255, 255, 255);
    gpu_core_set_debug_view(c, ESPGPU_DEBUG_VIEW_TILES);
    clear(rgb(0, 0, 0));
    /* tile (0,0)：30 个小三角形；tile (1,0)：一个盖满的大三角形 */
    static float tri[32 * 6];
    int n = 0;
    for (int k = 0; k < 30; k++) {
        const float px = 10 + (k % 6) * 8, py = 20 + (k / 6) * 8;
        const float p[3][2] = {{px, py}, {px + 4, py}, {px, py + 4}};
        for (int m = 0; m < 3; m++) {
            tri[n * 2] = p[m][0] / 160.0f - 1;
            tri[n * 2 + 1] = 1 - p[m][1] / 100.0f;
            n++;
        }
    }
    const float big[3][2] = {{64, 0}, {64 + 200, 0}, {64, 200}};
    for (int m = 0; m < 3; m++) {
        tri[n * 2] = big[m][0] / 160.0f - 1;
        tri[n * 2 + 1] = 1 - big[m][1] / 100.0f;
        n++;
    }
    draw_raw(ESPGPU_PRIM_TRIANGLES, tri, n, 8);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    {
        const uint16_t a = g_frame[4 * g_w + 30], b = g_frame[4 * g_w + 100];
        if ((a >> 11) < 28 || (b >> 11) > 4) {
            printf("FAIL tiles view: busy tile %04x, single-tri tile %04x\n", a, b);
            g_fails++;
        }
    }

    gpu_core_set_debug_view(c, ESPGPU_DEBUG_VIEW_OVERDRAW);
    gpu_core_set_size(c, 160, 100);
    clear(rgb(0, 0, 0));
    const float full[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    draw_raw(ESPGPU_PRIM_QUADS, full, 4, 8);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    /* overdraw = 1 → heat(1, 8)：深蓝，不是白色原画面 */
    if (g_w != 160 || g_frame[50 * g_w + 80] == 0xFFFF) {
        printf("FAIL debug view lost after resize (%dx%d, %04x)\n", g_w, g_h, g_frame[50 * g_w + 80]);
        g_fails++;
    }
    gpu_core_set_debug_view(c, ESPGPU_DEBUG_VIEW_NONE);
    gpu_core_set_size(c, 320, 200);
}

/* 深度只测不写的批次跳过写回后，深度缓冲内容仍然保留 */
static void test_depth_persist(gpu_core_t *c)
{
    mat(ESPGPU_MATRIX_MVP, k_ident);
    layout_pos(3);
    clear(rgb(0, 0, 0));
    pipeline_ex(ESPGPU_SHADE_FLAT, 1, 1, 0, 0, 0);
    const float nearq[4][3] = {{-1, -1, -0.5f}, {1, -1, -0.5f}, {1, 1, -0.5f}, {-1, 1, -0.5f}};
    const float farq[4][3] = {{-1, -1, 0.5f}, {1, -1, 0.5f}, {1, 1, 0.5f}, {-1, 1, 0.5f}};
    set_color(255, 0, 0, 255);
    draw_raw(ESPGPU_PRIM_QUADS, nearq, 4, 12);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    pipeline_ex(ESPGPU_SHADE_FLAT, 1, 0, 0, 0, 0); /* 只测不写 */
    set_color(0, 255, 0, 255);
    draw_raw(ESPGPU_PRIM_QUADS, farq, 4, 12);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    pipeline_ex(ESPGPU_SHADE_FLAT, 1, 1, 0, 0, 0);
    set_color(0, 0, 255, 255);
    draw_raw(ESPGPU_PRIM_QUADS, farq, 4, 12);
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
    expect("depth persists across test-only pass", 160, 100, 255, 0, 0, 8);
}

/* 深度测试函数可配的 PIPELINE（雾关、不剔除） */
static void pipeline_z(uint8_t shade, int dtest, int dwrite, uint8_t zfunc)
{
    uint8_t body[sizeof(espgpu_pipeline_t) + sizeof(espgpu_pipeline_fog_t) +
                 sizeof(espgpu_pipeline_ext_t)];
    espgpu_pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.mask = 0xFFFF & ~ESPGPU_PS_TEXTURE;
    p.depth_test = (uint8_t)dtest;
    p.depth_write = (uint8_t)dwrite;
    p.depth_func = zfunc;
    p.alpha_func = ESPGPU_CMP_ALWAYS;
    p.shade_mode = shade;
    espgpu_pipeline_fog_t f = {0, 1, 1};
    espgpu_pipeline_ext_t e;
    memset(&e, 0, sizeof(e));
    e.shade_model = ESPGPU_SHADE_MODEL_SMOOTH;
    e.color_write = 1;
    memcpy(body, &p, sizeof(p));
    memcpy(body + sizeof(p), &f, sizeof(f));
    memcpy(body + sizeof(p) + sizeof(f), &e, sizeof(e));
    push(ESPGPU_OP_PIPELINE, 0, body, sizeof(body));
}

/*
 * 多遍渲染：同一几何先走一条光栅路径写深度，再用 EQUAL 走另一条路径——每个像素都必须通过。
 * 各路径（fill / fast / px / generic）共用 row_zfix 的定点深度，任何一条自己算深度都会在这里漏像素。
 * 顺带检查分段统计（tris_px / tris_generic）有数。
 */
static void test_depth_equal_across_paths(gpu_core_t *c)
{
    float proj[16], mv[16], mvp[16];
    perspective(1.0f, 320.0f / 200.0f, 0.5f, 50.0f, proj);
    memcpy(mv, k_ident, sizeof(mv));
    mv[11] = -4.0f;
    mat_mul(proj, mv, mvp);
    /* 倾斜的四边形 + 一个小三角形：深度逐像素变化、透视非仿射、跨多个 tile */
    vtx_t q[7] = {
        {-2.5f, -1.2f, 1.5f, 0, 0, 200, 40, 40, 255},  {2.5f, -1.2f, 1.5f, 4, 0, 40, 200, 40, 255},
        {2.0f, 1.3f, -3.0f, 4, 4, 40, 40, 200, 255},   {-2.0f, 1.3f, -3.0f, 0, 4, 200, 200, 40, 255},
        {-0.3f, -0.2f, 0.7f, 0, 0, 90, 90, 90, 255},   {0.6f, 0.1f, 0.4f, 1, 0, 90, 90, 90, 255},
        {0.1f, 0.8f, -0.9f, 1, 1, 90, 90, 90, 255},
    };
    static uint16_t tex[8 * 8];
    for (int i = 0; i < 64; i++) tex[i] = (i & 1) ? rgb(120, 80, 40) : rgb(60, 100, 140);
    upload_tex(61, 8, 8, ESPGPU_FMT_RGB565, tex);
    mat(ESPGPU_MATRIX_MVP, mvp);
    layout_xyz_uv_rgba();
    /* 第一遍 / 第二遍的着色：TEX_MOD_COLOR + 插值色 + 纹理 → px；TEX_ADD → generic */
    const uint8_t sh[2][2] = {{ESPGPU_SHADE_TEX_MOD_COLOR, ESPGPU_SHADE_TEX_ADD},
                              {ESPGPU_SHADE_TEX_ADD, ESPGPU_SHADE_TEX_MOD_COLOR}};
    for (int order = 0; order < 2; order++) {
        clear(rgb(0, 0, 0));
        bind(0, 61, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT);
        pipeline_z(sh[order][0], 1, 1, ESPGPU_CMP_LESS);
        draw(ESPGPU_PRIM_QUADS, q, 4, NULL, 0);
        draw(ESPGPU_PRIM_TRIANGLES, q + 4, 3, NULL, 0);
        push(ESPGPU_OP_PRESENT, 0, NULL, 0);
        submit(c);
        int covered = 0;
        for (int i = 0; i < g_w * g_h; i++) covered += g_frame[i] != 0;
        espgpu_stats_t st;
        memset(&st, 0, sizeof(st));
        gpu_core_fill_stats(c, &st);
        const uint32_t first_px = st.tris_px, first_gen = st.tris_generic;

        /* 第二遍：不清屏，EQUAL、不写深度，纯白（无纹理的 TEX_ADD / 白色常量 × 白纹理） */
        bind(0, 0, ESPGPU_FILTER_NEAREST, ESPGPU_WRAP_REPEAT);
        pipeline_z(sh[order][1], 1, 0, ESPGPU_CMP_EQUAL);
        vtx_t w[7];
        memcpy(w, q, sizeof(w));
        for (int i = 0; i < 7; i++) w[i].r = w[i].g = w[i].b = 255;
        draw(ESPGPU_PRIM_QUADS, w, 4, NULL, 0);
        draw(ESPGPU_PRIM_TRIANGLES, w + 4, 3, NULL, 0);
        push(ESPGPU_OP_PRESENT, 0, NULL, 0);
        submit(c);
        int white = 0;
        for (int i = 0; i < g_w * g_h; i++) white += g_frame[i] == 0xFFFF;
        memset(&st, 0, sizeof(st));
        gpu_core_fill_stats(c, &st);
        if (covered < 5000 || white != covered) {
            printf("FAIL depth EQUAL across paths (order %d): %d covered, %d passed\n", order, covered,
                   white);
            g_fails++;
        }
        const bool px_used = order == 0 ? first_px > 0 : st.tris_px > 0;
        const bool gen_used = order == 0 ? st.tris_generic > 0 : first_gen > 0;
        if (!px_used || !gen_used) {
            printf("FAIL path stats (order %d): px %u/%u generic %u/%u\n", order, first_px, st.tris_px,
                   first_gen, st.tris_generic);
            g_fails++;
        }
    }
}

/* 坏输入：不能崩、不能越界、不能泄漏 */
static void test_bad_input(gpu_core_t *c)
{
    espgpu_stats_t st0, st1;
    memset(&st0, 0, sizeof(st0));
    gpu_core_fill_stats(c, &st0);

    /* 1. RGB565 首片 + RGBA4444 续片（旧代码写空的 alpha 平面） */
    {
        uint8_t body[sizeof(espgpu_upload_texture_t) + 64];
        espgpu_upload_texture_t u;
        memset(&u, 0, sizeof(u));
        u.id = 50;
        u.width = 8;
        u.height = 8;
        u.total_len = 128;
        u.format = ESPGPU_FMT_RGB565;
        memset(body, 0x5A, sizeof(body));
        memcpy(body, &u, sizeof(u));
        push(ESPGPU_OP_UPLOAD_TEXTURE, 0, body, (uint32_t)sizeof(u) + 64);
        u.offset = 64;
        u.format = ESPGPU_FMT_RGBA4444;
        memcpy(body, &u, sizeof(u));
        push(ESPGPU_OP_UPLOAD_TEXTURE, 0, body, (uint32_t)sizeof(u) + 64);
        submit(c);
    }
    /* 2. 巨大的 COPY_TO_TEXTURE（32 位 size_t 上 w*h*2 回绕）、id 0 的调色板 */
    {
        espgpu_fbo_t f;
        memset(&f, 0, sizeof(f));
        f.action = ESPGPU_FBO_COPY_TO_TEXTURE;
        f.texture_id = 51;
        f.width = 46341;
        f.height = 46341;
        push(ESPGPU_OP_FBO, 0, &f, sizeof(f));
        uint8_t pb[8 + 3];
        espgpu_tex_palette_t pal = {0, 0, 1};
        memcpy(pb, &pal, 8);
        pb[8] = pb[9] = 0;
        pb[10] = 255;
        push(ESPGPU_OP_TEX_PALETTE, 0, pb, sizeof(pb));
        submit(c);
    }
    memset(&st1, 0, sizeof(st1));
    gpu_core_fill_stats(c, &st1);
    if (st1.tex_resident != st0.tex_resident + 1) { /* 只有 50 */
        printf("FAIL bad input created textures: %u -> %u\n", st0.tex_resident, st1.tex_resident);
        g_fails++;
    }
    /* 3. 属性越过 stride：最后一个顶点不能读出命令缓冲（ASan 下验证） */
    {
        layout_pos(3);
        submit(c);
        espgpu_vertex_layout_t l;
        memset(&l, 0, sizeof(l));
        l.stride = 8;
        l.attrib_count = 1;
        l.attribs[0] = (espgpu_vertex_attrib_t){ESPGPU_ATTR_POSITION, ESPGPU_TYPE_F32, 3, 0, 0, 0, 0};
        push(ESPGPU_OP_VERTEX_LAYOUT, 0, &l, sizeof(l));
        submit(c);
        const uint32_t n = (uint32_t)(4 + sizeof(espgpu_draw_t) + 3 * 8);
        uint8_t *cmd = (uint8_t *)malloc(n);
        espgpu_cmd_t h = {ESPGPU_OP_DRAW, ESPGPU_DRAWF_3D, (uint16_t)(n - 4)};
        espgpu_draw_t d;
        memset(&d, 0, sizeof(d));
        d.prim = ESPGPU_PRIM_TRIANGLES;
        d.vertex_count = 3;
        memcpy(cmd, &h, 4);
        memcpy(cmd + 4, &d, sizeof(d));
        memset(cmd + 4 + sizeof(d), 0, 24);
        gpu_core_exec(c, cmd, n);
        free(cmd);
    }
    push(ESPGPU_OP_PRESENT, 0, NULL, 0);
    submit(c);
}

int main(void)
{
    gpu_core_config_t cfg = {2u << 20, 4u << 20, 1u << 20, 512};
    gpu_core_cb_t cb = {NULL, present, NULL};
    gpu_core_t *c = gpu_core_create(&cfg, &cb);
    if (!c) {
        printf("core create failed\n");
        return 1;
    }
    scene_2d(c);
    scene_3d(c, "core_3d.bmp", true);
    scene_vm(c);
    for (int v = 1; v < ESPGPU_DEBUG_VIEW_COUNT; v++) {
        char name[32];
        snprintf(name, sizeof(name), "core_debug%d.bmp", v);
        gpu_core_set_debug_view(c, (uint8_t)v);
        scene_3d(c, name, false);
    }
    gpu_core_set_debug_view(c, 0);
    test_coverage(c);
    test_persp_span_end(c);
    test_pipeline_rebinds_2d(c);
    test_vm_temps_zeroed(c);
    test_debug_views(c);
    test_depth_persist(c);
    test_depth_equal_across_paths(c);
    test_bad_input(c);
    espgpu_stats_t st;
    memset(&st, 0, sizeof(st));
    gpu_core_fill_stats(c, &st);
    printf("frames=%d tris=%u culled=%u fill=%u tex=%u (%u B) bins=%u B\n", g_frames,
           st.triangles, st.culled, st.fill_pixels, st.tex_resident, st.tex_bytes, st.bin_bytes);
    gpu_core_destroy(c);
    printf(g_fails ? "FAILED (%d)\n" : "OK\n", g_fails);
    return g_fails ? 1 : 0;
}
