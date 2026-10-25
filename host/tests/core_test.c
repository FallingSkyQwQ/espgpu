/*
 * core_test.c — 设备核心的主机端冒烟测试（不需要硬件）
 *
 * 直接把命令流喂给 firmware/core，把 PRESENT 出来的帧写成 BMP：
 *   core_2d.bmp   M2 的 2D 路径（棋盘背景 + alpha 精灵 + 进度条）
 *   core_3d.bmp   透视立方体（深度 / 高洛德 / 背面剔除）+ 透视校正地板纹理 + 雾 + 线
 *   core_vm.bmp   片元 VM 程序（等离子）+ FBO 渲染到纹理再贴回屏幕
 * 并检查若干像素的期望值。返回非 0 表示失败。
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
    espgpu_stats_t st;
    memset(&st, 0, sizeof(st));
    gpu_core_fill_stats(c, &st);
    printf("frames=%d tris=%u culled=%u fill=%u tex=%u (%u B) bins=%u B\n", g_frames,
           st.triangles, st.culled, st.fill_pixels, st.tex_resident, st.tex_bytes, st.bin_bytes);
    gpu_core_destroy(c);
    printf(g_fails ? "FAILED (%d)\n" : "OK\n", g_fails);
    return g_fails ? 1 : 0;
}
