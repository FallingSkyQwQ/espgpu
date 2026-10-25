/*
 * glsl_tests.cpp — 编译器单元测试（平台无关）
 *
 * 片元着色器用 VM 参考解释器执行，结果与手算的期望值比较。
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <functional>
#include <limits>
#include <random>

#include "espgpu_glsl.h"
#include "espgpu_proto.h"
#include "internal.h"

#if defined(GLSL_TESTS_FIRMWARE_VM)
/* 与设备同一份 VM 解释器（firmware/core/vm.c）做差分 */
#include "core_internal.h"
#endif

using namespace espgpu::glsl;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (c) {                                                                  \
            g_pass++;                                                             \
        } else {                                                                  \
            g_fail++;                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
        }                                                                         \
    } while (0)

static bool near(float a, float b, float eps = 1e-3f)
{
    return std::fabs(a - b) <= eps * (1.0f + std::fabs(b));
}

static Shader fs(const std::string &src)
{
    Shader s = compile(Stage::Fragment, src);
    if (!s.ok) std::printf("  [fs log] %s", s.log.c_str());
    return s;
}

static Shader vs(const std::string &src)
{
    Shader s = compile(Stage::Vertex, src);
    if (!s.ok) std::printf("  [vs log] %s", s.log.c_str());
    return s;
}

static int vreg_of_slot(int slot)
{
    switch (slot) {
    case ESPGPU_ATTR_COLOR: return 0;
    case ESPGPU_ATTR_TEXCOORD0: return 1;
    case ESPGPU_ATTR_TEXCOORD1: return 2;
    case ESPGPU_ATTR_TEXCOORD2: return 3;
    case ESPGPU_ATTR_TEXCOORD3: return 4;
    case ESPGPU_ATTR_NORMAL: return 5;
    default: return 7;
    }
}

struct Env {
    std::map<std::string, std::vector<float>> uniforms; /* 按叶子名，按槽 ×4 展开 */
    std::map<std::string, std::vector<float>> varyings; /* 按名字，4 个分量 */
    float fragcoord[4] = {0, 0, 0, 1};
};

static void tex_fn(int unit, float u, float v, float out[4], void *)
{
    /* 可预测的"纹理"：rgba = (u, v, unit, 1) */
    out[0] = u;
    out[1] = v;
    out[2] = static_cast<float>(unit);
    out[3] = 1.0f;
}

static bool run(const Shader &s, const Env &e, float out[4])
{
    float c[ESPGPU_VM_CONSTS][4] = {};
    float v[ESPGPU_VM_VARYINGS][4] = {};
    std::vector<std::array<float, 4>> un(static_cast<size_t>(std::max(1, s.uniform_slot_count)), {0, 0, 0, 0});
    for (const auto &u : s.uniforms) {
        auto it = e.uniforms.find(u.name);
        if (it == e.uniforms.end() || u.local_slot < 0) continue;
        for (size_t k = 0; k < it->second.size(); k++) un[static_cast<size_t>(u.local_slot) + k / 4][k % 4] = it->second[k];
    }
    fragment_consts(s, reinterpret_cast<const float(*)[4]>(un.data()), c);
    for (const auto &vs : s.frag.varying_slots) {
        auto it = e.varyings.find(s.inputs[static_cast<size_t>(vs.first)].name);
        if (it == e.varyings.end()) continue;
        const int r = vreg_of_slot(vs.second);
        for (size_t k = 0; k < 4 && k < it->second.size(); k++) v[r][k] = it->second[k];
    }
    for (int k = 0; k < 4; k++) v[6][k] = e.fragcoord[k];
    return vm_run(s.frag.code.data(), s.frag.code.size(), v, c, tex_fn, nullptr, out);
}

static bool has_error(const std::string &src, Stage st, const char *needle, int line = -1)
{
    Shader s = compile(st, src);
    if (s.ok) {
        std::printf("  expected error '%s' but compiled\n", needle);
        return false;
    }
    if (s.log.find(needle) == std::string::npos) {
        std::printf("  log was: %s", s.log.c_str());
        return false;
    }
    if (line > 0 && s.log.rfind(std::to_string(line) + ":", 0) != 0) {
        std::printf("  wrong line: %s", s.log.c_str());
        return false;
    }
    return true;
}

/* ── 预处理 ── */

static void test_preprocess()
{
    std::string out, err;
    int ver = 0;
    CHECK(preprocess("#version 300 es\n#define N 4\nfloat a[N];\n", out, err, &ver));
    CHECK(ver == 300);
    CHECK(out.find("float a[4];") != std::string::npos);

    CHECK(preprocess("#define SQ(x) ((x)*(x))\nfloat y = SQ(a+1);\n", out, err, nullptr));
    CHECK(out.find("((a+1)*(a+1))") != std::string::npos);

    CHECK(preprocess("#define CAT(a,b) a##b\nint CAT(foo,bar);\n", out, err, nullptr));
    CHECK(out.find("foobar") != std::string::npos);

    CHECK(preprocess("#if defined(GL_ES) && __VERSION__ >= 100\nyes\n#else\nno\n#endif\n", out, err, nullptr));
    CHECK(out.find("yes") != std::string::npos && out.find("no") == std::string::npos);

    CHECK(preprocess("#ifdef FOO\na\n#elif 1+1==2\nb\n#else\nc\n#endif\n", out, err, nullptr));
    CHECK(out.find('b') != std::string::npos && out.find('a') == std::string::npos);

    CHECK(!preprocess("#if 1\nx\n", out, err, nullptr));
    CHECK(err.find("missing #endif") != std::string::npos);
    CHECK(!preprocess("\n#error boom\n", out, err, nullptr));
    CHECK(err.rfind("2:", 0) == 0);

    /* 注释与续行不影响行号 */
    CHECK(preprocess("/* a\n b */\n#define X \\\n 1\nX\nfoo\n", out, err, nullptr));
    int nl = 0;
    for (char ch : out) nl += ch == '\n';
    CHECK(nl == 6);
}

/* ── 语法 / 类型错误 ── */

static void test_errors()
{
    CHECK(has_error("void main() { float x = 1.0 }", Stage::Fragment, "expected ';'", 1));
    CHECK(has_error("void main() {\n  vec3 a = 1.0;\n}", Stage::Fragment, "cannot initialize 'vec3' with 'float'", 2));
    CHECK(has_error("void main() { float a = 1.0 + 1; }", Stage::Fragment, "no implicit conversions"));
    CHECK(has_error("void main() { gl_FragColor = vec4(q); }", Stage::Fragment, "undeclared identifier 'q'"));
    CHECK(has_error("uniform float u;\nvoid main() { u = 1.0; }", Stage::Fragment, "cannot assign to uniform 'u'", 2));
    CHECK(has_error("void main() { vec2 v; v.xz = vec2(1.0); }", Stage::Fragment, "invalid swizzle"));
    CHECK(has_error("void main() { vec2 v; v.xx = vec2(1.0); }", Stage::Fragment, "repeated components"));
    CHECK(has_error("float f(float x) { return f(x); }\nvoid main() { gl_FragColor = vec4(f(1.0)); }", Stage::Fragment, "recursion"));
    CHECK(has_error("uniform float n;\nvoid main() { float s = 0.0; for (float i = 0.0; i < n; i += 1.0) s += i; gl_FragColor = vec4(s); }",
                    Stage::Fragment, "constant trip count"));
    CHECK(has_error("void main() { switch (1) { } }", Stage::Fragment, "switch"));
    CHECK(has_error("void main() {}\nvoid main2() { int x = 1.0; }", Stage::Fragment, "cannot initialize 'int'"));
    CHECK(has_error("#version 300 es\nprecision mediump float;\nin vec2 uv;\nout vec4 o;\nuniform sampler2D s;\nvoid main(){ o = dFdx(uv).xyxy; }",
                    Stage::Fragment, "not supported"));
    CHECK(has_error("void main() { gl_Position = vec4(1.0); discard; }", Stage::Vertex, "discard"));
    CHECK(has_error("void notmain() {}", Stage::Vertex, "missing main"));
}

/* ── 模式识别 ── */

static const char *kPre300 = "#version 300 es\nprecision mediump float;\n";

static void test_patterns()
{
    {
        Shader s = fs(std::string(kPre300) + "in vec4 vColor;\nout vec4 o;\nvoid main(){ o = vColor; }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_FLAT && s.frag.color_input == 0);
    }
    {
        Shader s = fs(std::string(kPre300) + "in vec2 uv;\nuniform sampler2D tex;\nout vec4 o;\nvoid main(){ o = texture(tex, uv); }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_TEX);
        CHECK(s.frag.unit_sampler.size() == 1 && s.frag.varying_slots.size() == 1 &&
              s.frag.varying_slots[0].second == ESPGPU_ATTR_TEXCOORD0);
    }
    {
        Shader s = fs(std::string(kPre300) + "in vec2 uv; in vec4 c;\nuniform sampler2D tex;\nout vec4 o;\nvoid main(){ vec4 t = texture(tex, uv); o = c * t; }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_TEX_MOD_COLOR && s.frag.color_input == 1);
    }
    {
        Shader s = fs(std::string(kPre300) + "in vec2 a; in vec2 b;\nuniform sampler2D t0, t1;\nout vec4 o;\nvoid main(){ o = texture(t0, a) * texture(t1, b); }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_TEX_MOD_TEX && s.frag.color_white);
        CHECK(s.frag.unit_sampler.size() == 2);
    }
    {
        /* ES 1.00 + alpha 测试 */
        Shader s = fs("precision mediump float;\nvarying vec2 uv;\nuniform sampler2D tex;\nvoid main(){\n vec4 c = texture2D(tex, uv);\n if (c.a < 0.5) discard;\n gl_FragColor = c;\n}");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_TEX && s.frag.alpha_test &&
              s.frag.alpha_func == ESPGPU_CMP_GEQUAL && s.frag.alpha_ref == 128);
    }
    {
        Shader s = fs(std::string(kPre300) + "uniform vec4 color;\nin vec2 uv;\nuniform sampler2D tex;\nout vec4 o;\nvoid main(){ o = texture(tex, uv) * color; }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_TEX_MOD_COLOR && s.frag.color_slots.size() == 1 &&
              s.frag.color_slots[0] == 0 && s.frag.color_input < 0);
    }
    {
        /* 常量乘 1 被化简掉，仍然是模式 */
        Shader s = fs(std::string(kPre300) + "in vec4 c;\nout vec4 o;\nvoid main(){ o = c * 1.0; }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_FLAT);
    }
    {
        /* 顶点色 × 常量：常量由运行时乘进顶点色，仍是 FLAT */
        Shader s = fs(std::string(kPre300) + "in vec4 c;\nout vec4 o;\nvoid main(){ o = c * 0.5; }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_FLAT && s.frag.color_input == 0 &&
              s.frag.color_slots.size() == 1);
    }
}

/* ── VM 执行 ── */

static void test_vm()
{
    {
        /* 动画 plasma：sin + 均匀量 + 常量 */
        Shader s = fs(std::string(kPre300) +
                      "uniform float time;\nin vec2 p;\nout vec4 o;\n"
                      "void main(){\n"
                      "  float v = sin(p.x * 10.0 + time) + cos(p.y * 7.0 - time * 0.5);\n"
                      "  o = vec4(0.5 + 0.5 * v, abs(p.x - p.y), fract(v * 3.0), 1.0);\n"
                      "}");
        CHECK(s.ok && s.frag.program);
        Env e;
        e.uniforms["time"] = {1.3f, 0, 0, 0};
        e.varyings["p"] = {0.25f, 0.7f, 0, 0};
        float o[4];
        CHECK(run(s, e, o));
        const float v = std::sin(0.25f * 10 + 1.3f) + std::cos(0.7f * 7 - 0.65f);
        CHECK(near(o[0], 0.5f + 0.5f * v));
        CHECK(near(o[1], 0.45f));
        CHECK(near(o[2], 3 * v - std::floor(3 * v)));
        CHECK(near(o[3], 1.0f));
    }
    {
        /* if/else 谓词化 + 函数 + out 参数 */
        Shader s = fs(std::string(kPre300) +
                      "in vec2 p;\nout vec4 o;\n"
                      "void split(float x, out float lo, out float hi) { if (x < 0.5) { lo = x; hi = 0.0; } else { lo = 0.0; hi = x; } }\n"
                      "float sq(float x) { return x * x; }\n"
                      "void main(){ float a, b; split(p.x, a, b); o = vec4(a, b, sq(p.y), 1.0); }");
        CHECK(s.ok);
        float o[4];
        Env e;
        e.varyings["p"] = {0.3f, 0.6f, 0, 0};
        CHECK(run(s, e, o) && near(o[0], 0.3f) && near(o[1], 0.0f) && near(o[2], 0.36f));
        e.varyings["p"] = {0.8f, 0.2f, 0, 0};
        CHECK(run(s, e, o) && near(o[0], 0.0f) && near(o[1], 0.8f) && near(o[2], 0.04f));
    }
    {
        /* 循环展开 + break + 早 return */
        Shader s = fs(std::string(kPre300) +
                      "in vec2 p;\nout vec4 o;\n"
                      "float f(float x) { if (x > 0.9) return 9.0; float s = 0.0; for (int i = 0; i < 8; i++) { if (float(i) > x * 10.0) break; s += 1.0; } return s; }\n"
                      "void main(){ o = vec4(f(p.x), 0.0, 0.0, 1.0); }");
        CHECK(s.ok);
        float o[4];
        Env e;
        e.varyings["p"] = {0.35f, 0, 0, 0};
        CHECK(run(s, e, o) && near(o[0], 4.0f));
        e.varyings["p"] = {0.95f, 0, 0, 0};
        CHECK(run(s, e, o) && near(o[0], 9.0f));
        e.varyings["p"] = {0.0f, 0, 0, 0};
        CHECK(run(s, e, o) && near(o[0], 1.0f));
    }
    {
        /* discard 条件 */
        Shader s = fs(std::string(kPre300) +
                      "in vec2 p;\nout vec4 o;\nvoid main(){ if (length(p - vec2(0.5)) > 0.4) discard; o = vec4(1.0); }");
        CHECK(s.ok && s.frag.program);
        float o[4];
        Env e;
        e.varyings["p"] = {0.5f, 0.5f, 0, 0};
        CHECK(run(s, e, o));
        e.varyings["p"] = {0.0f, 0.0f, 0, 0};
        CHECK(!run(s, e, o));
    }
    {
        /* 结构体、数组、矩阵、动态下标 */
        Shader s = fs(std::string(kPre300) +
                      "struct L { vec3 dir; float k; };\n"
                      "uniform L light;\nuniform mat3 m;\nuniform vec4 pal[4];\n"
                      "in vec3 n; in vec2 t;\nout vec4 o;\n"
                      "void main(){\n"
                      "  vec3 nn = m * n;\n"
                      "  float d = max(dot(normalize(nn), light.dir), 0.0) * light.k;\n"
                      "  int i = int(t.x * 3.99);\n"
                      "  o = vec4(d, pal[i].yz, 1.0);\n"
                      "}");
        CHECK(s.ok && s.frag.program);
        Env e;
        e.uniforms["light.dir"] = {0, 0, 1, 0};
        e.uniforms["light.k"] = {2, 0, 0, 0};
        /* mat3 按列：第 0 列 (0,1,0)，第 1 列 (1,0,0)，第 2 列 (0,0,1) —— 交换 x/y */
        e.uniforms["m"] = {0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
        e.uniforms["pal"] = {0, 1, 2, 0, 0, 3, 4, 0, 0, 5, 6, 0, 0, 7, 8, 0};
        e.varyings["n"] = {0.6f, 0, 0.8f, 0};
        e.varyings["t"] = {0.6f, 0, 0, 0};
        float o[4];
        CHECK(run(s, e, o));
        CHECK(near(o[0], 1.6f));
        CHECK(near(o[1], 5.0f) && near(o[2], 6.0f));
    }
    {
        /* 纹理 + 混合 + gl_FragCoord + smoothstep / mix / step / clamp */
        Shader s = fs(std::string(kPre300) +
                      "uniform sampler2D a; uniform sampler2D b;\nin vec2 uv;\nout vec4 o;\n"
                      "void main(){\n"
                      "  vec4 x = texture(a, uv); vec4 y = texture(b, uv.yx);\n"
                      "  float t = smoothstep(0.2, 0.8, gl_FragCoord.x / 100.0);\n"
                      "  o = mix(x, y, t) + vec4(step(0.5, uv.x), clamp(uv.y * 4.0, 0.0, 1.0), 0.0, 0.0);\n"
                      "}");
        CHECK(s.ok && s.frag.program && s.frag.unit_sampler.size() == 2);
        Env e;
        e.varyings["uv"] = {0.7f, 0.1f, 0, 0};
        e.uniforms["@fragcoord"] = {1, 1, 0, 0};
        e.fragcoord[0] = 50;
        float o[4];
        CHECK(run(s, e, o));
        const float t = 0.5f; /* smoothstep(0.2,0.8,0.5) */
        CHECK(near(o[0], 0.7f * (1 - t) + 0.1f * t + 1.0f));
        CHECK(near(o[1], 0.1f * (1 - t) + 0.7f * t + 0.4f));
        CHECK(near(o[2], 0.0f * (1 - t) + 1.0f * t));
    }
    {
        /* gl_FragCoord：设备左上原点 → GL 左下原点 + 窗口缩放 */
        Shader s = fs(std::string(kPre300) + "out vec4 o;\nvoid main(){ o = vec4(gl_FragCoord.xy, 0.0, 1.0); }");
        CHECK(s.ok && s.frag.program);
        Env e;
        e.uniforms["@fragcoord"] = {2, -2, 0, 400}; /* 设备 320x200，窗口 640x400：y = 400 - 2·v6.y */
        e.fragcoord[0] = 10.5f;
        e.fragcoord[1] = 0.5f;
        float o[4];
        CHECK(run(s, e, o) && near(o[0], 21.0f) && near(o[1], 399.0f));
        Shader v = vs("#version 300 es\nin vec4 p;\nvoid main(){ gl_Position = p; }");
        Program p = link(v, s);
        CHECK(p.ok && p.visible_uniforms == 0 && p.uniforms.size() == 1);
    }
    {
        /* atan / pow / mod / 三元 / 整数运算 */
        Shader s = fs(std::string(kPre300) +
                      "in vec2 p;\nout vec4 o;\n"
                      "void main(){\n"
                      "  int k = int(p.x * 10.0); int q = k / 3; int r = k % 3;\n"
                      "  float a = atan(p.y, p.x);\n"
                      "  o = vec4(a, pow(p.x, 2.0), mod(p.y * 5.0, 1.5), p.x > p.y ? float(q) : float(r));\n"
                      "}");
        CHECK(s.ok);
        Env e;
        e.varyings["p"] = {0.75f, -0.4f, 0, 0};
        float o[4];
        CHECK(run(s, e, o));
        CHECK(near(o[0], std::atan2(-0.4f, 0.75f), 1e-3f));
        CHECK(near(o[1], 0.5625f));
        CHECK(near(o[2], -2.0f - 1.5f * std::floor(-2.0f / 1.5f)));
        CHECK(near(o[3], 2.0f));
    }
    {
        /* 嵌套循环 + continue + while + 局部数组动态下标写 + 结构体构造 + const 数组 */
        Shader s = fs(std::string(kPre300) +
                      "struct P { float a; vec2 b; };\n"
                      "const float W[3] = float[3](0.25, 0.5, 0.25);\n"
                      "in vec2 p;\nout vec4 o;\n"
                      "void main(){\n"
                      "  float acc = 0.0;\n"
                      "  for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) { if (j == 1) continue; acc += W[i] * float(j); } }\n"
                      "  int k = 0; float w = 0.0; while (k < 4) { w += 1.0; k++; }\n"
                      "  float arr[4]; for (int i = 0; i < 4; i++) arr[i] = float(i);\n"
                      "  int d = int(p.x * 3.99); arr[d] = 10.0;\n"
                      "  P q = P(arr[1], vec2(acc, w));\n"
                      "  o = vec4(q.a, q.b, arr[2]);\n"
                      "}");
        CHECK(s.ok);
        Env e;
        e.varyings["p"] = {0.3f, 0, 0, 0}; /* d = 1 */
        float o[4];
        CHECK(run(s, e, o) && near(o[0], 10.0f) && near(o[1], 2.0f) && near(o[2], 4.0f) && near(o[3], 2.0f));
        e.varyings["p"] = {0.6f, 0, 0, 0}; /* d = 2 */
        CHECK(run(s, e, o) && near(o[0], 1.0f) && near(o[3], 10.0f));
    }
    {
        /* ES 1.00 走 VM：texture2D + gl_FragColor + 预处理宏 */
        Shader s = fs("#define SCALE 2.0\nprecision mediump float;\nvarying vec2 uv;\nuniform sampler2D t;\n"
                      "void main(){ gl_FragColor = texture2D(t, uv * SCALE) + vec4(0.0, 0.0, 0.0, -0.5); }");
        CHECK(s.ok && s.frag.program);
        Env e;
        e.varyings["uv"] = {0.1f, 0.2f, 0, 0};
        float o[4];
        CHECK(run(s, e, o) && near(o[0], 0.2f) && near(o[1], 0.4f) && near(o[3], 0.5f));
    }
    {
        /* 反汇编不崩 */
        Shader s = fs(std::string(kPre300) + "in vec4 c;\nout vec4 o;\nvoid main(){ o = c.bgra * 0.5; }");
        CHECK(s.ok && s.frag.program);
        const std::string d = vm_disassemble(s.frag.code.data(), s.frag.code.size());
        CHECK(d.find("mul") != std::string::npos && d.find("end") != std::string::npos);
    }
}

/* ── 顶点着色器 ── */

static void test_vertex()
{
    {
        Shader s = vs("#version 300 es\nuniform mat4 uP; uniform mat4 uMV;\nlayout(location=0) in vec3 aPos;\nin vec2 aUV;\nout vec2 vUV;\n"
                      "void main(){ vUV = aUV; gl_Position = uP * uMV * vec4(aPos, 1.0); }");
        CHECK(s.ok && s.vert.tnl);
        CHECK(s.vert.mvp_uniforms.size() == 2 && s.vert.pos_components == 3 && s.vert.pos_input == 0);
        CHECK(s.vert.varying_input.size() == 1 && s.vert.varying_input[0] == 1);
        CHECK(s.inputs[0].location == 0);
    }
    {
        /* 非直通：主机解释执行 */
        Shader s = vs("#version 300 es\nuniform float t;\nin vec2 aPos;\nout vec3 vC;\n"
                      "void main(){ float c = cos(t), sn = sin(t); vec2 r = mat2(c, sn, -sn, c) * aPos;\n"
                      "  vC = vec3(aPos * 0.5 + 0.5, 1.0); gl_Position = vec4(r, 0.0, 1.0); }");
        CHECK(s.ok && !s.vert.tnl);
        float in[2][4] = {{1, 0, 0, 1}, {0, 0, 0, 1}};
        float un[1][4] = {{1.5707964f, 0, 0, 0}};
        float pos[4];
        float var[1][4];
        run_vertex(s, in, un, pos, var);
        CHECK(near(pos[0], 0.0f, 1e-3f) && near(pos[1], 1.0f) && near(pos[3], 1.0f));
        CHECK(near(var[0][0], 1.0f) && near(var[0][1], 0.5f) && near(var[0][2], 1.0f));
    }
    {
        /* 链接 */
        Shader v = vs("uniform mat4 m; uniform vec4 tint;\nattribute vec4 p; attribute vec4 c;\nvarying vec4 vc;\nvoid main(){ vc = c; gl_Position = m * p; }");
        Shader f = fs("precision mediump float;\nuniform vec4 tint;\nvarying vec4 vc;\nvoid main(){ gl_FragColor = vc * tint; }");
        CHECK(v.ok && f.ok && v.vert.tnl && v.vert.pos_components == 4);
        Program p = link(v, f);
        CHECK(p.ok && p.uniforms.size() == 2 && p.vs_out_for_fs_in[0] == 0);
        CHECK(p.uniforms[1].vs_leaf == 1 && p.uniforms[1].fs_leaf == 0);
        Shader f2 = fs("precision mediump float;\nvarying vec3 vc;\nvoid main(){ gl_FragColor = vec4(vc, 1.0); }");
        Program p2 = link(v, f2);
        CHECK(!p2.ok && p2.log.find("different types") != std::string::npos);
    }
}


/* ── 回归：本轮修复的问题 ── */

static Shader fs_quiet(const std::string &src) { return compile(Stage::Fragment, src); }

static void test_regressions()
{
    std::string out, err;
    /* CRLF 续行（Windows 源码） */
    CHECK(preprocess("#define A 1 + \\\r\n 2\r\nint x = A;\r\n", out, err, nullptr));
    CHECK(out.find("int x = 1 + 2;") != std::string::npos);
    /* #if 短路：右侧除零不报错；三元 */
    CHECK(preprocess("#if 0 && (1 / 0)\na\n#elif 1 || (2 % 0)\nb\n#endif\n", out, err, nullptr));
    CHECK(out.find('b') != std::string::npos && out.find('a') == std::string::npos);
    CHECK(preprocess("#if (1 ? 2 : 3) == 2\nyes\n#endif\n", out, err, nullptr) && out.find("yes") != std::string::npos);
    CHECK(!preprocess("#if 1 / 0\n#endif\n", out, err, nullptr));
    /* #ifdef 认识预定义宏 */
    CHECK(preprocess("#ifdef __VERSION__\nv\n#endif\n#ifdef GL_ES\ne\n#endif\n", out, err, nullptr));
    CHECK(out.find('v') != std::string::npos && out.find('e') != std::string::npos);
    /* #if 里的大移位 / 溢出不是 UB */
    CHECK(preprocess("#if (1 << 70) == 0 && (9223372036854775807 + 1) < 0\nok\n#endif\n", out, err, nullptr));
    /* 超出 32 位的整数字面量报错 */
    CHECK(has_error("void main(){ int a = 4294967296; }", Stage::Fragment, "invalid integer literal"));
    CHECK(has_error("void main(){ int a = 09; }", Stage::Fragment, "invalid integer literal"));

    /* alpha 测试参考值逐字节等价（原来 `a <= 0.1` 给 GREATER 26，会多丢 alpha = 26 的片元） */
    struct AT {
        const char *cond;
        uint8_t func, ref;
    } at[] = {
        {"c.a < 0.5", ESPGPU_CMP_GEQUAL, 128},  {"c.a <= 0.1", ESPGPU_CMP_GEQUAL, 26},
        {"c.a > 0.9", ESPGPU_CMP_LEQUAL, 229},  {"c.a >= 0.5", ESPGPU_CMP_LEQUAL, 127},
        {"0.25 > c.a", ESPGPU_CMP_GEQUAL, 64},  {"c.a < 0.0", ESPGPU_CMP_ALWAYS, 0},
        {"c.a < 2.0", ESPGPU_CMP_NEVER, 0},
    };
    for (const auto &a : at) {
        Shader s = fs("precision mediump float;\nvarying vec2 uv;\nuniform sampler2D tex;\nvoid main(){ vec4 c = texture2D(tex, uv); if (" +
                      std::string(a.cond) + ") discard; gl_FragColor = c; }");
        const bool ok = s.ok && !s.frag.program && s.frag.alpha_test && s.frag.alpha_func == a.func && s.frag.alpha_ref == a.ref;
        if (!ok) std::printf("  alpha '%s': func %d ref %d\n", a.cond, s.frag.alpha_func, s.frag.alpha_ref);
        CHECK(ok);
    }

    /* 更多内置模式：常量色、3 分量顶点色、纹理 × vec4(vc, 1)、顶点色 × uniform、纹理 × uniform 表达式 */
    {
        Shader s = fs(std::string(kPre300) + "out vec4 o;\nvoid main(){ o = vec4(1.0, 0.5, 0.0, 1.0); }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_FLAT && s.frag.color_input < 0 && s.frag.color_slots.size() == 1);
        float c[ESPGPU_VM_CONSTS][4] = {};
        float un[1][4] = {{0, 0, 0, 0}};
        fragment_consts(s, un, c);
        const float *k = c[s.frag.color_slots[0]];
        CHECK(near(k[0], 1) && near(k[1], 0.5f) && near(k[2], 0) && near(k[3], 1));
    }
    {
        Shader s = fs("precision mediump float;\nvarying vec3 vColor;\nvoid main(){ gl_FragColor = vec4(vColor, 1.0); }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_FLAT && s.frag.color_input == 0 && s.frag.color_slots.empty());
    }
    {
        Shader s = fs("precision mediump float;\nvarying vec2 uv; varying vec3 vc; uniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv) * vec4(vc, 1.0); }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_TEX_MOD_COLOR && s.frag.color_input == 1);
    }
    {
        Shader s = fs("precision mediump float;\nvarying vec4 vc; uniform vec4 tint;\nvoid main(){ gl_FragColor = vc * tint; }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_FLAT && s.frag.color_input == 0 &&
              s.frag.color_slots.size() == 1 && s.frag.color_slots[0] == 0);
    }
    {
        Shader s = fs("precision mediump float;\nvarying vec2 uv; uniform sampler2D t; uniform vec4 col; uniform float k;\n"
                      "void main(){ gl_FragColor = texture2D(t, uv) * (col * k); }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_TEX_MOD_COLOR && s.frag.color_input < 0 &&
              s.frag.color_slots.size() == 1 && s.frag.preshader_nodes >= 1);
        if (s.ok && !s.frag.color_slots.empty()) {
            float c[ESPGPU_VM_CONSTS][4] = {};
            std::vector<std::array<float, 4>> un(static_cast<size_t>(s.uniform_slot_count), {0, 0, 0, 0});
            for (const auto &u : s.uniforms) {
                if (u.name == "col") un[static_cast<size_t>(u.local_slot)] = {0.2f, 0.4f, 0.6f, 0.8f};
                if (u.name == "k") un[static_cast<size_t>(u.local_slot)] = {0.5f, 0, 0, 0};
            }
            fragment_consts(s, reinterpret_cast<const float(*)[4]>(un.data()), c);
            const float *k = c[s.frag.color_slots[0]];
            CHECK(near(k[0], 0.1f) && near(k[1], 0.2f) && near(k[2], 0.3f) && near(k[3], 0.4f));
        }
    }
    {
        /* 纹理 alpha 被换掉时不能当内置模式 */
        Shader s = fs("precision mediump float;\nvarying vec2 uv; uniform sampler2D t;\nvoid main(){ gl_FragColor = vec4(texture2D(t, uv).rgb, 1.0); }");
        CHECK(s.ok && s.frag.program && s.frag.code.size() == 2); /* tex o0.xyz + end：alpha 用 o0 初值 1 */
    }
    {
        /* 4 分量顶点色的 alpha 被换掉：不是 FLAT */
        Shader s = fs("precision mediump float;\nvarying vec4 vc;\nvoid main(){ gl_FragColor = vec4(vc.rgb, 1.0); }");
        CHECK(s.ok && s.frag.program);
    }

    /* 指令数：demo plasma 原来 45 条 */
    {
        Shader s = fs("#version 300 es\nprecision mediump float;\nuniform float uTime;\nuniform vec2 uRes;\nin vec2 vUV;\nout vec4 fragColor;\n"
                      "void main() {\n vec2 p = vUV * 2.0 - 1.0;\n p.x *= uRes.x / uRes.y;\n float v = sin(p.x * 3.0 + uTime);\n"
                      " v += sin((p.y * 3.0 + uTime) * 0.5);\n v += sin((p.x * 2.0 + p.y * 2.0 + uTime) * 0.7);\n"
                      " vec2 c = p + vec2(sin(uTime * 0.3), cos(uTime * 0.5)) * 0.8;\n v += sin(sqrt(dot(c, c) * 8.0 + 1.0) + uTime);\n"
                      " v *= 0.5;\n vec3 col = vec3(0.5 + 0.5 * sin(3.14159 * v), 0.5 + 0.5 * sin(3.14159 * v + 2.094), 0.5 + 0.5 * sin(3.14159 * v + 4.188));\n"
                      " fragColor = vec4(col, 1.0);\n}\n");
        CHECK(s.ok && s.frag.program && s.frag.code.size() <= 25);
        if (s.ok) std::printf("  plasma: %zu VM instructions (incl. END), %d preshader ops\n", s.frag.code.size(), s.frag.preshader_nodes);
    }

    /* 顶点着色器：批量执行与逐个执行一致；uniform 部分只算一次 */
    {
        Shader v = vs("#version 300 es\nin vec2 aPos;\nin vec2 aUV;\nuniform float uTime;\nuniform float uAspect;\nout vec2 vUV;\n"
                      "void main() {\n float a = uTime * 0.7;\n mat2 r = mat2(cos(a), sin(a), -sin(a), cos(a));\n vec2 p = r * (aPos * 0.45);\n"
                      " p.y += 0.08 * sin(uTime * 2.0 + aPos.x * 3.0);\n p.x /= uAspect;\n vUV = aUV;\n gl_Position = vec4(p, 0.0, 1.0);\n}\n");
        CHECK(v.ok && !v.vert.tnl);
        float un[2][4] = {{1.3f, 0, 0, 0}, {1.6f, 0, 0, 0}};
        float in[3][2][4] = {{{0.5f, -0.25f, 0, 1}, {0, 1, 0, 1}}, {{-1, 1, 0, 1}, {1, 0, 0, 1}}, {{0.1f, 0.2f, 0, 1}, {0.3f, 0.4f, 0, 1}}};
        float vout[3][2][4];
        run_vertex_batch(v, un, 3, &in[0][0], &vout[0][0]);
        bool same = true;
        for (int i = 0; i < 3; i++) {
            float pos[4], var[1][4];
            run_vertex(v, in[i], un, pos, var);
            for (int k = 0; k < 4; k++) same = same && pos[k] == vout[i][0][k] && var[0][k] == vout[i][1][k];
        }
        CHECK(same);
        const float a = 1.3f * 0.7f;
        const float px = (std::cos(a) * 0.5f * 0.45f - std::sin(a) * -0.25f * 0.45f) / 1.6f;
        CHECK(near(vout[0][0][0], px));
    }

    /* 寄存器压力：16 个以上同时存活的 vec4 仍能编译（打包 / 重试） */
    {
        std::string src = std::string(kPre300) + "in vec4 a;\nout vec4 o;\nvoid main(){\n";
        for (int i = 0; i < 18; i++) src += " vec4 t" + std::to_string(i) + " = sin(a * " + std::to_string(i + 1) + ".0);\n";
        src += " o = ";
        for (int i = 0; i < 18; i++) src += (i ? " * t" : "t") + std::to_string(17 - i);
        src += ";\n}\n";
        Shader s = fs_quiet(src);
        CHECK(s.ok);
    }
}

/* ── 差分测试：C++ 直接算 vs 参考 VM vs 固件 VM ───────────────────── */

using V4 = std::array<float, 4>;

struct DCase {
    const char *name;
    const char *helpers;
    const char *body;
    std::function<bool(const V4 &p, const V4 &q, const V4 &u, const V4 &w, const V4 &fc, V4 &o)> ref;
    float tol = 2e-4f;
    bool fragcoord = false;
};

static void tex_one(int, float, float, float out[4], void *)
{
    out[0] = out[1] = out[2] = out[3] = 1.0f; /* 与固件未绑定纹理的单元一致 */
}

/** 跑一遍：参考 VM 与（可用时）固件 VM，regs 跨调用保留（设备在像素间不清零 temps） */
static bool run_dual(const Shader &s, const Env &e, float out[4], float fw_regs[ESPGPU_VM_TEMPS * 4], bool &fw_ok)
{
    float c[ESPGPU_VM_CONSTS][4] = {};
    float v[ESPGPU_VM_VARYINGS][4] = {};
    std::vector<std::array<float, 4>> un(static_cast<size_t>(std::max(1, s.uniform_slot_count)), {0, 0, 0, 0});
    for (const auto &u : s.uniforms) {
        auto it = e.uniforms.find(u.name);
        if (it == e.uniforms.end() || u.local_slot < 0) continue;
        for (size_t k = 0; k < it->second.size(); k++) un[static_cast<size_t>(u.local_slot) + k / 4][k % 4] = it->second[k];
    }
    fragment_consts(s, reinterpret_cast<const float(*)[4]>(un.data()), c);
    for (const auto &vs : s.frag.varying_slots) {
        auto it = e.varyings.find(s.inputs[static_cast<size_t>(vs.first)].name);
        if (it == e.varyings.end()) continue;
        const int r = vreg_of_slot(vs.second);
        for (size_t k = 0; k < 4 && k < it->second.size(); k++) v[r][k] = it->second[k];
    }
    for (int k = 0; k < 4; k++) v[6][k] = e.fragcoord[k];
    const bool kept = vm_run(s.frag.code.data(), s.frag.code.size(), v, c, tex_one, nullptr, out);
    fw_ok = true;
#if defined(GLSL_TESTS_FIRMWARE_VM)
    static gp_prog_t prog;
    std::memset(&prog, 0, sizeof(prog));
    prog.count = static_cast<uint16_t>(s.frag.code.size());
    prog.varyings = s.frag.varying_count;
    prog.valid = 1;
    std::memcpy(prog.code, s.frag.code.data(), s.frag.code.size() * sizeof(espgpu_vm_insn_t));
    if (!gp_vm_validate(prog.code, prog.count)) {
        std::printf("  firmware rejected the program\n");
        fw_ok = false;
        return kept;
    }
    gp_sampler_t smp[ESPGPU_MAX_TEXTURE_UNITS];
    std::memset(smp, 0, sizeof(smp));
    float fo[4];
    const bool fkept = gp_vm_run(&prog, &c[0][0], smp, &v[0][0], fw_regs, fo);
    if (fkept != kept) fw_ok = false;
    for (int k = 0; k < 4 && kept && fkept; k++) {
        if (!(std::fabs(fo[k] - out[k]) <= 1e-5f * (1.0f + std::fabs(out[k])))) fw_ok = false;
    }
    if (!fw_ok) std::printf("  firmware %d (%g %g %g %g) vs ref %d (%g %g %g %g)\n", fkept, fo[0], fo[1], fo[2], fo[3], kept,
                            out[0], out[1], out[2], out[3]);
#else
    (void)fw_regs;
#endif
    return kept;
}

static float gmod(float x, float y) { return x - y * std::floor(x / y); }
static float gsmooth(float a, float b, float x)
{
    float t = std::min(std::max((x - a) / (b - a), 0.0f), 1.0f);
    return t * t * (3 - 2 * t);
}
static float gsign(float x) { return x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f); }
static float gclamp(float x, float a, float b) { return std::min(std::max(x, a), b); }
static float gtrunc_div(int a, int b) { return static_cast<float>(a / b); }

static void test_differential()
{
    std::vector<DCase> cases = {
        {"arith", "", "o = vec4(p.x*q.x + p.y, p.z - p.w*q.y, (p.x + u.x)*w.x, -p.y*2.0 + 1.0);",
         [](auto &p, auto &q, auto &u, auto &w, auto &, V4 &o) {
             o = {p[0] * q[0] + p[1], p[2] - p[3] * q[1], (p[0] + u[0]) * w[0], -p[1] * 2 + 1};
             return true;
         }},
        {"mix_step_smooth_clamp", "", "o = vec4(mix(p.x, p.y, q.x*0.5), step(p.y, p.x), smoothstep(-1.0, 1.0, p.z), clamp(p.w, -0.5, 0.5));",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             const float t = q[0] * 0.5f;
             o = {p[0] * (1 - t) + p[1] * t, p[0] < p[1] ? 0.0f : 1.0f, gsmooth(-1, 1, p[2]), gclamp(p[3], -0.5f, 0.5f)};
             return true;
         }},
        {"mod_fract_floor_ceil", "", "o = vec4(mod(p.x*3.0, q.y), fract(p.y*2.5), floor(p.z*1.7), ceil(p.w*1.3));",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             o = {gmod(p[0] * 3, q[1]), p[1] * 2.5f - std::floor(p[1] * 2.5f), std::floor(p[2] * 1.7f), std::ceil(p[3] * 1.3f)};
             return true;
         }},
        {"trunc_round_sign_abs", "", "o = vec4(trunc(p.x*2.3), round(p.y*2.2), sign(p.z), abs(p.w) + roundEven(q.x*3.0));",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             o = {std::trunc(p[0] * 2.3f), std::floor(p[1] * 2.2f + 0.5f), gsign(p[2]), std::fabs(p[3]) + std::nearbyint(q[0] * 3)};
             return true;
         }},
        {"atan_asin_acos", "", "o = vec4(atan(p.y, p.x), atan(p.z * 3.0), asin(clamp(p.w, -1.0, 1.0) * 0.9), acos(clamp(p.x, -1.0, 1.0) * 0.9));",
         [](auto &p, auto &, auto &, auto &, auto &, V4 &o) {
             o = {std::atan2(p[1], p[0]), std::atan(p[2] * 3), std::asin(gclamp(p[3], -1, 1) * 0.9f), std::acos(gclamp(p[0], -1, 1) * 0.9f)};
             return true;
         },
         5e-4f},
        {"exp_log_pow", "", "o = vec4(exp(p.x), log(q.x), pow(q.y, p.z), exp2(p.w) + log2(q.z));",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             o = {std::exp(p[0]), std::log(q[0]), std::pow(q[1], p[2]), std::exp2(p[3]) + std::log2(q[2])};
             return true;
         }},
        {"pow_const", "", "o = vec4(pow(q.x, 2.0), pow(q.y, 3.0), pow(q.z, 4.0), pow(q.w, 1.0) + pow(q.x, 0.5));",
         [](auto &, auto &q, auto &, auto &, auto &, V4 &o) {
             o = {q[0] * q[0], q[1] * q[1] * q[1], q[2] * q[2] * q[2] * q[2], q[3] + std::sqrt(q[0])};
             return true;
         }},
        {"geometry", "", "vec3 n = normalize(p.xyz + vec3(0.0, 0.0, 3.0)); o = vec4(length(p.xy), distance(p.zw, q.xy), dot(n, q.xyz), inversesqrt(q.w));",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             const float nx = p[0], ny = p[1], nz = p[2] + 3, l = std::sqrt(nx * nx + ny * ny + nz * nz);
             o = {std::hypot(p[0], p[1]), std::hypot(p[2] - q[0], p[3] - q[1]), (nx * q[0] + ny * q[1] + nz * q[2]) / l, 1 / std::sqrt(q[3])};
             return true;
         }},
        {"reflect_refract_faceforward", "",
         "vec3 i = normalize(p.xyz + vec3(0.1, 0.2, 3.0)); vec3 n = normalize(q.xyz); vec3 r = reflect(i, n);"
         " vec3 t = refract(i, n, 0.7); vec3 f = faceforward(n, i, p.xyz); o = vec4(r.x + t.y, t.z, f.x + f.y, r.z);",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             float i[3] = {p[0] + 0.1f, p[1] + 0.2f, p[2] + 3};
             float n[3] = {q[0], q[1], q[2]};
             auto norm = [](float *v) { const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); for (int k = 0; k < 3; k++) v[k] /= l; };
             norm(i);
             norm(n);
             const float d = n[0] * i[0] + n[1] * i[1] + n[2] * i[2];
             float r[3], t[3], f[3];
             for (int k = 0; k < 3; k++) r[k] = i[k] - 2 * d * n[k];
             const float eta = 0.7f, kk = 1 - eta * eta * (1 - d * d);
             for (int k = 0; k < 3; k++) t[k] = kk < 0 ? 0 : eta * i[k] - (eta * d + std::sqrt(kk)) * n[k];
             const float dd = p[0] * i[0] + p[1] * i[1] + p[2] * i[2];
             for (int k = 0; k < 3; k++) f[k] = dd < 0 ? n[k] : -n[k];
             o = {r[0] + t[1], t[2], f[0] + f[1], r[2]};
             return true;
         }},
        {"matrix", "",
         "mat2 m = mat2(p.x, p.y, p.z, p.w); mat3 m3 = mat3(q.xyz, p.xyz, u.xyz); vec2 a = m * q.xy; vec2 b = q.xy * m;"
         " vec3 c = m3 * w.xyz; mat2 mm = m * mat2(q); o = vec4(a.x + b.y, c.y, mm[1][0], transpose(m)[0][1] + matrixCompMult(m, m)[1][1]);",
         [](auto &p, auto &q, auto &u, auto &w, auto &, V4 &o) {
             /* m 列主序：col0 = (p.x, p.y)，col1 = (p.z, p.w) */
             const float ax = p[0] * q[0] + p[2] * q[1];
             const float by = q[0] * p[2] + q[1] * p[3];
             const float cy = q[1] * w[0] + p[1] * w[1] + u[1] * w[2];
             /* mm = m * Q，Q col0 = (q.x, q.y)，col1 = (q.z, q.w)；mm[1][0] = 第 1 列第 0 行 */
             const float mm10 = p[0] * q[2] + p[2] * q[3];
             o = {ax + by, cy, mm10, p[2] + p[3] * p[3]}; /* transpose(m)[0][1] = m[1][0] */
             return true;
         }},
        {"int_semantics", "",
         "int a = int(p.x*7.0); int b = int(q.x*3.0) + 1; int c = int(q.y*5.0);"
         " o = vec4(float(a / b), float(c % b), float(a * 2 - b), float(-a / 2) + float(int(-p.y * 3.0)));",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             const int a = static_cast<int>(p[0] * 7), b = static_cast<int>(q[0] * 3) + 1, c = static_cast<int>(q[1] * 5);
             o = {gtrunc_div(a, b), static_cast<float>(c % b), static_cast<float>(a * 2 - b),
                  gtrunc_div(-a, 2) + static_cast<float>(static_cast<int>(-p[1] * 3))};
             return true;
         }},
        {"bool_short_circuit", "",
         "float s = 0.0; bool b1 = p.x > 0.0; if (b1 && (s += 1.0) > 0.0) s += 2.0; if (p.y > 0.0 || (s += 10.0) > 0.0) s += 100.0;"
         " o = vec4(s, b1 ? p.z : p.w, float(p.x < p.y), float(!(p.z >= p.w)) + float(b1 ^^ (p.w > 0.0)));",
         [](auto &p, auto &, auto &, auto &, auto &, V4 &o) {
             float s = 0;
             const bool b1 = p[0] > 0;
             if (b1) s += 3;
             if (p[1] > 0) s += 100;
             else s += 110;
             o = {s, b1 ? p[2] : p[3], p[0] < p[1] ? 1.0f : 0.0f, (!(p[2] >= p[3]) ? 1.0f : 0.0f) + ((b1 != (p[3] > 0)) ? 1.0f : 0.0f)};
             return true;
         }},
        {"compound_incdec_swizzle", "",
         "vec4 v = p; v.zx += q.xy; v.yw *= 2.0; v.x -= w.x; float c = q.x; float d = c++; float e = ++c; c--; v.y /= q.z;"
         " o = v + vec4(d, e, c, 0.0);",
         [](auto &p, auto &q, auto &, auto &w, auto &, V4 &o) {
             float v[4] = {p[0], p[1], p[2], p[3]};
             v[2] += q[0];
             v[0] += q[1];
             v[1] *= 2;
             v[3] *= 2;
             v[0] -= w[0];
             float c = q[0];
             const float d = c;
             c += 1;
             c += 1;
             const float e = c;
             c -= 1;
             v[1] /= q[2];
             o = {v[0] + d, v[1] + e, v[2] + c, v[3]};
             return true;
         }},
        {"struct_array_index", "struct S { vec2 a; float b[3]; };",
         "S s; s.a = p.xy; s.b[0] = q.x; s.b[1] = q.y; s.b[2] = q.z; int i = int(clamp(p.z * 1.5 + 1.5, 0.0, 2.99)); s.b[i] += 5.0;"
         " float arr[4] = float[4](p.w, q.w, u.x, w.x); int j = int(q.x * 1.9);"
         " o = vec4(s.a.y + s.b[1], s.b[i], arr[j] + arr[3], s.b[2 - i]);",
         [](auto &p, auto &q, auto &u, auto &w, auto &, V4 &o) {
             float b[3] = {q[0], q[1], q[2]};
             const int i = static_cast<int>(gclamp(p[2] * 1.5f + 1.5f, 0, 2.99f));
             b[i] += 5;
             const float arr[4] = {p[3], q[3], u[0], w[0]};
             const int j = static_cast<int>(q[0] * 1.9f);
             o = {p[1] + b[1], b[i], arr[j] + arr[3], b[2 - i]};
             return true;
         }},
        {"loop_break_continue_return",
         "float f(float x) { float s = 0.0; for (int i = 0; i < 6; i++) { if (float(i) > x * 3.0 + 3.0) break; if (i == 2) continue; s += float(i); } if (x > 1.5) return -1.0; return s; }",
         "o = vec4(f(p.x), f(p.y), 0.0, 1.0);",
         [](auto &p, auto &, auto &, auto &, auto &, V4 &o) {
             auto f = [](float x) {
                 float s = 0;
                 for (int i = 0; i < 6; i++) {
                     if (static_cast<float>(i) > x * 3 + 3) break;
                     if (i == 2) continue;
                     s += static_cast<float>(i);
                 }
                 return x > 1.5f ? -1.0f : s;
             };
             o = {f(p[0]), f(p[1]), 0, 1};
             return true;
         }},
        {"discard_in_loop", "", "for (int i = 0; i < 3; i++) { if (p[i] > 1.5) discard; } o = p;",
         [](auto &p, auto &, auto &, auto &, auto &, V4 &o) {
             o = p;
             return !(p[0] > 1.5f || p[1] > 1.5f || p[2] > 1.5f);
         }},
        {"discard_zero_compare", "", "if (p.x < 0.0) discard; if (0.0 < p.y - 1.8) discard; o = q;",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             o = q;
             return !(p[0] < 0 || p[1] - 1.8f > 0);
         }},
        {"sel_to_cmp", "",
         "float a = p.x < 0.0 ? q.x : q.y; float b = p.y >= 0.0 ? q.z : q.w; float c = 0.0 < p.z ? u.x : u.y; float d = 0.0 >= p.w ? w.x : w.y; o = vec4(a, b, c, d);",
         [](auto &p, auto &q, auto &u, auto &w, auto &, V4 &o) {
             o = {p[0] < 0 ? q[0] : q[1], p[1] >= 0 ? q[2] : q[3], p[2] > 0 ? u[0] : u[1], p[3] <= 0 ? w[0] : w[1]};
             return true;
         }},
        {"uniform_only_preshader", "",
         "float k = sin(u.x * 3.0) / w.y; vec2 t = vec2(cos(u.y), u.z * u.w); o = vec4(p.x * k + t.x, (p.y + u.x) * w.x, p.z / w.z, t.y - p.w);",
         [](auto &p, auto &, auto &u, auto &w, auto &, V4 &o) {
             const float k = std::sin(u[0] * 3) / w[1];
             o = {p[0] * k + std::cos(u[1]), (p[1] + u[0]) * w[0], p[2] / w[2], u[2] * u[3] - p[3]};
             return true;
         }},
        {"clamp_to_sat", "", "o = vec4(clamp(p.x * q.x + p.y, 0.0, 1.0), clamp(dot(p.xy, q.xy), 0.0, 1.0), clamp(p.z, 0.0, 1.0), 1.0);",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             o = {gclamp(p[0] * q[0] + p[1], 0, 1), gclamp(p[0] * q[0] + p[1] * q[1], 0, 1), gclamp(p[2], 0, 1), 1};
             return true;
         }},
        {"slp_shape", "", "float v = p.x * q.x + p.y; o = vec4(0.5 + 0.5 * sin(v), 0.5 + 0.5 * sin(v + 2.0), 0.5 + 0.5 * sin(v + 4.0), 1.0);",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             const float v = p[0] * q[0] + p[1];
             o = {0.5f + 0.5f * std::sin(v), 0.5f + 0.5f * std::sin(v + 2), 0.5f + 0.5f * std::sin(v + 4), 1};
             return true;
         }},
        {"fragcoord", "", "o = vec4(gl_FragCoord.xy * 0.01, gl_FragCoord.z, gl_FragCoord.w);",
         [](auto &, auto &, auto &, auto &, auto &fc, V4 &o) {
             /* @fragcoord = (2, -2, 0, 400) */
             o = {(fc[0] * 2) * 0.01f, (fc[1] * -2 + 400) * 0.01f, fc[2], fc[3]};
             return true;
         },
         2e-4f, true},
        {"hyperbolic_tan", "", "o = vec4(sinh(p.x), cosh(p.y), tanh(p.z), tan(p.w * 0.7));",
         [](auto &p, auto &, auto &, auto &, auto &, V4 &o) {
             o = {std::sinh(p[0]), std::cosh(p[1]), std::tanh(p[2]), std::tan(p[3] * 0.7f)};
             return true;
         },
         1e-3f},
        {"bvec_minmax", "", "bvec2 bb = lessThan(p.xy, q.xy); o = vec4(any(bb) ? 1.0 : 0.0, all(bb) ? 1.0 : 0.0, min(p.z, q.z), max(p.w, -q.w));",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             const bool b0 = p[0] < q[0], b1 = p[1] < q[1];
             o = {(b0 || b1) ? 1.0f : 0.0f, (b0 && b1) ? 1.0f : 0.0f, std::min(p[2], q[2]), std::max(p[3], -q[3])};
             return true;
         }},
        {"register_pressure", "",
         "vec4 a = p*q, b = p+q, c = p-q, d = p*u, e = q*w, f = sin(p), g = cos(q), h = p*w;"
         " o = a*b + c*d + e*f + g*h + (a+c)*(e-g) + (b-d)*(f+h);",
         [](auto &p, auto &q, auto &u, auto &w, auto &, V4 &o) {
             for (int k = 0; k < 4; k++) {
                 const float a = p[k] * q[k], b = p[k] + q[k], c = p[k] - q[k], d = p[k] * u[k], e = q[k] * w[k];
                 const float f = std::sin(p[k]), g = std::cos(q[k]), h = p[k] * w[k];
                 o[k] = a * b + c * d + e * f + g * h + (a + c) * (e - g) + (b - d) * (f + h);
             }
             return true;
         },
         1e-3f},
        {"neg_folding", "", "o = vec4(-p.x * q.x + 1.0, -(p.y - q.y), q.z - p.z * q.w, -dot(-p.xy, q.xy));",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             o = {-p[0] * q[0] + 1, -(p[1] - q[1]), q[2] - p[2] * q[3], p[0] * q[0] + p[1] * q[1]};
             return true;
         }},
        {"mad_fusion_neg", "", "o = vec4(p.x * q.x - p.y, p.z - p.w * q.y, -(p.x * q.z) + u.x, u.y - (-p.y) * q.w);",
         [](auto &p, auto &q, auto &u, auto &, auto &, V4 &o) {
             o = {p[0] * q[0] - p[1], p[2] - p[3] * q[1], -(p[0] * q[2]) + u[0], u[1] + p[1] * q[3]};
             return true;
         }},
        {"ctor_mat_vec", "",
         "mat3 m = mat3(p.x); vec4 v = vec4(mat2(p.x, p.y, p.z, p.w)); mat2 m2 = mat2(m); mat4 m4 = mat4(m2);"
         " o = vec4(m[1][1] + m4[3][3] + m4[2][0], v.y + v.z, m2[0][0] + m2[0][1], determinant(mat2(p.x, p.y, p.z, p.w)));",
         [](auto &p, auto &, auto &, auto &, auto &, V4 &o) {
             o = {p[0] + 1 + 0, p[1] + p[2], p[0] + 0, p[0] * p[3] - p[2] * p[1]};
             return true;
         }},
        {"nested_if_else", "",
         "float r = 0.0; if (p.x > 0.0) { r = q.x; if (p.y > 0.0) r += 1.0; else r -= 1.0; } else { r = -q.y; } o = vec4(r, 0.0, 0.0, 1.0);",
         [](auto &p, auto &q, auto &, auto &, auto &, V4 &o) {
             float r;
             if (p[0] > 0) r = q[0] + (p[1] > 0 ? 1.0f : -1.0f);
             else r = -q[1];
             o = {r, 0, 0, 1};
             return true;
         }},
        {"early_return_main", "", "o = vec4(1.0); if (p.x > 0.5) { o = vec4(2.0); return; } o.x = 3.0;",
         [](auto &p, auto &, auto &, auto &, auto &, V4 &o) {
             o = p[0] > 0.5f ? V4{2, 2, 2, 2} : V4{3, 1, 1, 1};
             return true;
         }},
        {"one_minus_smoothstep", "", "o = vec4(1.0 - smoothstep(-0.5, 0.5, p.x), 1.0 - smoothstep(0.2, 1.2, p.yz), 1.0);",
         [](auto &p, auto &, auto &, auto &, auto &, V4 &o) {
             o = {1 - gsmooth(-0.5f, 0.5f, p[0]), 1 - gsmooth(0.2f, 1.2f, p[1]), 1 - gsmooth(0.2f, 1.2f, p[2]), 1};
             return true;
         }},
        {"while_do_loops", "",
         "float s = 0.0; int k = 0; while (k < 3) { s += p[k]; k++; } int m = 0; do { s *= 0.5; m++; } while (m < 2); o = vec4(s, float(k), float(m), 1.0);",
         [](auto &p, auto &, auto &, auto &, auto &, V4 &o) {
             const float s = (p[0] + p[1] + p[2]) * 0.25f;
             o = {s, 3, 2, 1};
             return true;
         }},
    };

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> sym(-2.0f, 2.0f), pos(0.1f, 2.0f), un1(-1.0f, 1.0f);
    /* -1 = 自动；0..7 = 逐个优化组合（每种组合都必须正确） */
    for (int mode = -1; mode < 8; mode++) {
    g_codegen_force_mode = mode;
    for (const auto &c : cases) {
        const std::string src = std::string("#version 300 es\nprecision highp float;\nin vec4 p; in vec4 q;\nuniform vec4 u; uniform vec4 w;\nout vec4 o;\n") +
                                c.helpers + "\nvoid main(){ " + c.body + " }\n";
        Shader s = compile(Stage::Fragment, src);
        if (!s.ok) {
            std::printf("  [%s] %s", c.name, s.log.c_str());
            CHECK(s.ok);
            continue;
        }
        if (!s.frag.program) continue; /* 内置模式不跑 VM */
        float regs[ESPGPU_VM_TEMPS * 4];
        for (float &r : regs) r = std::numeric_limits<float>::quiet_NaN();
        int bad = 0, fw_bad = 0;
        for (int it = 0; it < 200; it++) {
            V4 p, q, u, w, fc;
            for (int k = 0; k < 4; k++) {
                p[static_cast<size_t>(k)] = sym(rng);
                q[static_cast<size_t>(k)] = pos(rng);
                u[static_cast<size_t>(k)] = un1(rng);
                w[static_cast<size_t>(k)] = pos(rng);
            }
            fc = {std::floor(pos(rng) * 100) + 0.5f, std::floor(pos(rng) * 80) + 0.5f, pos(rng) * 0.5f, pos(rng)};
            Env e;
            e.varyings["p"] = {p[0], p[1], p[2], p[3]};
            e.varyings["q"] = {q[0], q[1], q[2], q[3]};
            e.uniforms["u"] = {u[0], u[1], u[2], u[3]};
            e.uniforms["w"] = {w[0], w[1], w[2], w[3]};
            e.uniforms["@fragcoord"] = {2, -2, 0, 400};
            for (int k = 0; k < 4; k++) e.fragcoord[k] = fc[static_cast<size_t>(k)];
            float out[4];
            bool fw_ok = true;
            const bool kept = run_dual(s, e, out, regs, fw_ok);
            V4 want{};
            const bool wkept = c.ref(p, q, u, w, fc, want);
            bool ok = kept == wkept;
            for (int k = 0; k < 4 && ok && kept; k++) ok = near(out[k], want[static_cast<size_t>(k)], c.tol);
            if (!ok && bad++ < 2) {
                std::printf("  [%s] p=(%g %g %g %g) q=(%g %g %g %g) got %d (%g %g %g %g) want %d (%g %g %g %g)\n", c.name, p[0], p[1],
                            p[2], p[3], q[0], q[1], q[2], q[3], kept, out[0], out[1], out[2], out[3], wkept, want[0], want[1],
                            want[2], want[3]);
            }
            fw_bad += !fw_ok;
        }
        if (bad || fw_bad) {
            std::printf("  [%s mode %d] %d mismatches, %d firmware mismatches\n%s", c.name, mode, bad, fw_bad,
                        vm_disassemble(s.frag.code.data(), s.frag.code.size()).c_str());
        }
        CHECK(bad == 0);
        CHECK(fw_bad == 0);
    }
    }
    g_codegen_force_mode = -1;
}

/* ── 随机表达式：C++ 求值 vs 参考 VM vs 固件 VM，全部优化组合 ───────── */

namespace {

struct Ev {
    V4 v{};
    bool unstable = false; /* 输入落在不连续点附近（floor / step / 比较）：这组样本不比 */
};

struct Ctx {
    V4 p, q, u, w;
};

using Fn = std::function<Ev(const Ctx &)>;

struct Gx {
    std::string s;
    int n; /* 1..4 */
    Fn f;
};

struct Gen {
    std::mt19937 rng;
    explicit Gen(unsigned seed) : rng(seed) {}
    int ri(int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); }

    static std::string ty(int n) { return n == 1 ? "float" : "vec" + std::to_string(n); }
    static constexpr float kEps = 1e-3f;

    Gx leaf(int n)
    {
        static const char *names[4] = {"p", "q", "u", "w"};
        const int which = ri(0, 4);
        if (which == 4) {
            float k = static_cast<float>(ri(-20, 20)) / 8.0f;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", k);
            std::string str = n == 1 ? std::string(buf) : ty(n) + "(" + buf + ")";
            return {str, n, [k](const Ctx &) { Ev e; e.v = {k, k, k, k}; return e; }};
        }
        static const char comp[] = "xyzw";
        int sel[4];
        std::string sw;
        for (int i = 0; i < n; i++) {
            sel[i] = ri(0, 3);
            sw += comp[sel[i]];
        }
        return {std::string(names[which]) + "." + sw, n, [which, sel, n](const Ctx &c) {
                    const V4 &src = which == 0 ? c.p : which == 1 ? c.q : which == 2 ? c.u : c.w;
                    Ev e;
                    for (int i = 0; i < 4; i++) e.v[static_cast<size_t>(i)] = src[static_cast<size_t>(sel[std::min(i, n - 1)])];
                    return e;
                }};
    }

    template <typename Op>
    static Fn cw2(Fn a, Fn b, int n, Op op)
    {
        return [a, b, n, op](const Ctx &c) {
            Ev x = a(c), y = b(c), r;
            r.unstable = x.unstable || y.unstable;
            for (int i = 0; i < n; i++) r.v[static_cast<size_t>(i)] = op(x.v[static_cast<size_t>(i)], y.v[static_cast<size_t>(i)], r.unstable);
            return r;
        };
    }
    template <typename Op>
    static Fn cw1(Fn a, int n, Op op)
    {
        return [a, n, op](const Ctx &c) {
            Ev x = a(c), r;
            r.unstable = x.unstable;
            for (int i = 0; i < n; i++) r.v[static_cast<size_t>(i)] = op(x.v[static_cast<size_t>(i)], r.unstable);
            return r;
        };
    }

    Gx gen(int n, int depth)
    {
        if (depth <= 0) return leaf(n);
        const int k = ri(0, 15);
        switch (k) {
        case 0: case 1: {
            Gx a = gen(n, depth - 1), b = gen(n, depth - 1);
            return {"(" + a.s + " + " + b.s + ")", n, cw2(a.f, b.f, n, [](float x, float y, bool &) { return x + y; })};
        }
        case 2: {
            Gx a = gen(n, depth - 1), b = gen(n, depth - 1);
            return {"(" + a.s + " - " + b.s + ")", n, cw2(a.f, b.f, n, [](float x, float y, bool &) { return x - y; })};
        }
        case 3: case 4: {
            Gx a = gen(n, depth - 1), b = gen(n, depth - 1);
            return {"(" + a.s + " * " + b.s + ")", n, cw2(a.f, b.f, n, [](float x, float y, bool &) { return x * y; })};
        }
        case 5: {
            Gx a = gen(n, depth - 1), b = gen(n, depth - 1);
            return {"(" + a.s + " / (abs(" + b.s + ") + 0.5))", n,
                    cw2(a.f, b.f, n, [](float x, float y, bool &) { return x / (std::fabs(y) + 0.5f); })};
        }
        case 6: {
            Gx a = gen(n, depth - 1), b = gen(n, depth - 1);
            const bool mn = ri(0, 1);
            return {std::string(mn ? "min(" : "max(") + a.s + ", " + b.s + ")", n,
                    cw2(a.f, b.f, n, [mn](float x, float y, bool &) { return mn ? std::min(x, y) : std::max(x, y); })};
        }
        case 7: {
            Gx a = gen(n, depth - 1);
            const int f = ri(0, 3);
            static const char *nm[4] = {"sin", "cos", "abs", "fract"};
            return {std::string(nm[f]) + "(" + a.s + ")", n, cw1(a.f, n, [f](float x, bool &un) {
                        if (f == 3 && std::fabs(x - std::round(x)) < kEps) un = true;
                        return f == 0 ? std::sin(x) : f == 1 ? std::cos(x) : f == 2 ? std::fabs(x) : x - std::floor(x);
                    })};
        }
        case 8: {
            Gx a = gen(n, depth - 1);
            return {"clamp(" + a.s + ", -1.0, 1.0)", n, cw1(a.f, n, [](float x, bool &) { return std::min(std::max(x, -1.0f), 1.0f); })};
        }
        case 9: {
            Gx a = gen(n, depth - 1), b = gen(n, depth - 1), t = gen(n, depth - 1);
            Fn fa = a.f, fb = b.f, ft = t.f;
            return {"mix(" + a.s + ", " + b.s + ", " + t.s + ")", n, [fa, fb, ft, n](const Ctx &c) {
                        Ev x = fa(c), y = fb(c), z = ft(c), r;
                        r.unstable = x.unstable || y.unstable || z.unstable;
                        for (int i = 0; i < n; i++) {
                            const size_t j = static_cast<size_t>(i);
                            r.v[j] = x.v[j] + z.v[j] * (y.v[j] - x.v[j]);
                        }
                        return r;
                    }};
        }
        case 10: {
            Gx a = gen(n, depth - 1), b = gen(n, depth - 1);
            return {"step(" + a.s + ", " + b.s + ")", n, cw2(a.f, b.f, n, [](float e, float x, bool &un) {
                        if (std::fabs(x - e) < kEps) un = true;
                        return x < e ? 0.0f : 1.0f;
                    })};
        }
        case 11: {
            /* 标量条件的三元 */
            Gx c1 = gen(1, depth - 1), c2 = gen(1, depth - 1), a = gen(n, depth - 1), b = gen(n, depth - 1);
            const int cmp = ri(0, 3);
            static const char *op[4] = {" < ", " <= ", " > ", " >= "};
            Fn f1 = c1.f, f2 = c2.f, fa = a.f, fb = b.f;
            return {"((" + c1.s + op[cmp] + c2.s + ") ? " + a.s + " : " + b.s + ")", n, [f1, f2, fa, fb, cmp](const Ctx &c) {
                        Ev x = f1(c), y = f2(c);
                        const float l = x.v[0], r = y.v[0];
                        const bool t = cmp == 0 ? l < r : cmp == 1 ? l <= r : cmp == 2 ? l > r : l >= r;
                        Ev o = t ? fa(c) : fb(c);
                        o.unstable = o.unstable || x.unstable || y.unstable || std::fabs(l - r) < kEps;
                        return o;
                    }};
        }
        case 12: {
            if (n != 1) break;
            const int m = ri(2, 4);
            Gx a = gen(m, depth - 1), b = gen(m, depth - 1);
            Fn fa = a.f, fb = b.f;
            return {"dot(" + a.s + ", " + b.s + ")", 1, [fa, fb, m](const Ctx &c) {
                        Ev x = fa(c), y = fb(c), r;
                        r.unstable = x.unstable || y.unstable;
                        float s = 0;
                        for (int i = 0; i < m; i++) s += x.v[static_cast<size_t>(i)] * y.v[static_cast<size_t>(i)];
                        r.v = {s, s, s, s};
                        return r;
                    }};
        }
        case 13: {
            /* 由标量拼成向量 */
            if (n == 1) break;
            std::vector<Gx> parts;
            std::string str = ty(n) + "(";
            for (int i = 0; i < n; i++) {
                parts.push_back(gen(1, depth - 1));
                str += (i ? ", " : "") + parts.back().s;
            }
            str += ")";
            std::vector<Fn> fs;
            for (auto &g : parts) fs.push_back(g.f);
            return {str, n, [fs](const Ctx &c) {
                        Ev r;
                        for (size_t i = 0; i < fs.size(); i++) {
                            Ev e = fs[i](c);
                            r.v[i] = e.v[0];
                            r.unstable = r.unstable || e.unstable;
                        }
                        return r;
                    }};
        }
        case 14: {
            /* 向量取分量 */
            Gx a = gen(4, depth - 1);
            static const char comp[] = "xyzw";
            int sel[4];
            std::string sw;
            for (int i = 0; i < n; i++) {
                sel[i] = ri(0, 3);
                sw += comp[sel[i]];
            }
            Fn fa = a.f;
            return {"(" + a.s + ")." + sw, n, [fa, sel, n](const Ctx &c) {
                        Ev x = fa(c), r;
                        r.unstable = x.unstable;
                        for (int i = 0; i < 4; i++) r.v[static_cast<size_t>(i)] = x.v[static_cast<size_t>(sel[std::min(i, n - 1)])];
                        return r;
                    }};
        }
        default: {
            /* a * b + c：MAD 融合 / 重结合 */
            Gx a = gen(n, depth - 1), b = gen(n, depth - 1), c = leaf(n);
            Fn fa = a.f, fb = b.f, fc = c.f;
            return {"(" + a.s + " * " + b.s + " + " + c.s + ")", n, [fa, fb, fc, n](const Ctx &cx) {
                        Ev x = fa(cx), y = fb(cx), z = fc(cx), r;
                        r.unstable = x.unstable || y.unstable || z.unstable;
                        for (int i = 0; i < n; i++) {
                            const size_t j = static_cast<size_t>(i);
                            r.v[j] = x.v[j] * y.v[j] + z.v[j];
                        }
                        return r;
                    }};
        }
        }
        return gen(n, depth - 1);
    }
};

} // namespace

static void test_random_expressions()
{
    std::mt19937 rng(777);
    std::uniform_real_distribution<float> sym(-2.0f, 2.0f);
    int compiled = 0, skipped = 0;
    /* GLSL_FUZZ=N 可以跑更多（默认 120 个着色器 × 9 种优化组合） */
    const char *env = std::getenv("GLSL_FUZZ");
    const int count = env ? std::max(1, std::atoi(env)) : 120;
    const int depth = env ? 4 : 3;
    for (int t = 0; t < count; t++) {
        Gen g(1000u + static_cast<unsigned>(t));
        /* 两个局部变量（共享子表达式 / 寄存器压力）+ 带写掩码的赋值 + 输出 */
        Gx a = g.gen(4, depth), b = g.gen(1, depth), c = g.gen(2, depth);
        Gx o = g.gen(4, depth - 1);
        const std::string src = std::string("#version 300 es\nprecision highp float;\nin vec4 p; in vec4 q;\nuniform vec4 u; uniform vec4 w;\nout vec4 o;\n") +
                                "void main(){\n vec4 ta = " + a.s + ";\n float tb = " + b.s + ";\n ta.yw = ta.yw * tb + " + c.s +
                                ";\n o = " + o.s + " + ta;\n}\n";
        Fn fa = a.f, fb = b.f, fc = c.f, fo = o.f;
        auto ref = [&](const Ctx &cx, V4 &out) {
            Ev ea = fa(cx), eb = fb(cx), ec = fc(cx), eo = fo(cx);
            V4 ta = ea.v;
            ta[1] = ta[1] * eb.v[0] + ec.v[0];
            ta[3] = ta[3] * eb.v[0] + ec.v[1];
            for (int k = 0; k < 4; k++) out[static_cast<size_t>(k)] = eo.v[static_cast<size_t>(k)] + ta[static_cast<size_t>(k)];
            return !(ea.unstable || eb.unstable || ec.unstable || eo.unstable);
        };
        for (int mode = -1; mode < 8; mode++) {
            g_codegen_force_mode = mode;
            Shader s = compile(Stage::Fragment, src);
            if (!s.ok) {
                /* 太长 / 寄存器不够都算合法的拒绝；其它错误是 bug */
                const bool limit = s.log.find("too long") != std::string::npos || s.log.find("temporaries") != std::string::npos ||
                                   s.log.find("exceed") != std::string::npos;
                if (!limit) std::printf("  [rand %d mode %d] %s%s", t, mode, s.log.c_str(), src.c_str());
                CHECK(limit);
                skipped++;
                continue;
            }
            if (!s.frag.program) continue;
            compiled++;
            float regs[ESPGPU_VM_TEMPS * 4];
            for (float &r : regs) r = std::numeric_limits<float>::quiet_NaN();
            int bad = 0, fw_bad = 0;
            for (int it = 0; it < 40; it++) {
                Ctx cx;
                for (int k = 0; k < 4; k++) {
                    cx.p[static_cast<size_t>(k)] = sym(rng);
                    cx.q[static_cast<size_t>(k)] = sym(rng);
                    cx.u[static_cast<size_t>(k)] = sym(rng);
                    cx.w[static_cast<size_t>(k)] = sym(rng);
                }
                Env e;
                e.varyings["p"] = {cx.p[0], cx.p[1], cx.p[2], cx.p[3]};
                e.varyings["q"] = {cx.q[0], cx.q[1], cx.q[2], cx.q[3]};
                e.uniforms["u"] = {cx.u[0], cx.u[1], cx.u[2], cx.u[3]};
                e.uniforms["w"] = {cx.w[0], cx.w[1], cx.w[2], cx.w[3]};
                V4 want{};
                const bool stable = ref(cx, want);
                float out[4];
                bool fw_ok = true;
                run_dual(s, e, out, regs, fw_ok);
                fw_bad += !fw_ok;
                if (!stable) continue;
                bool ok = true;
                for (int k = 0; k < 4; k++) {
                    const float wv = want[static_cast<size_t>(k)];
                    ok = ok && std::fabs(out[k] - wv) <= 2e-3f * (1.0f + std::fabs(wv));
                }
                if (!ok && bad++ == 0) {
                    std::printf("  [rand %d mode %d] got (%g %g %g %g) want (%g %g %g %g)\n%s%s", t, mode, out[0], out[1], out[2],
                                out[3], want[0], want[1], want[2], want[3], src.c_str(),
                                vm_disassemble(s.frag.code.data(), s.frag.code.size()).c_str());
                }
            }
            CHECK(bad == 0);
            CHECK(fw_bad == 0);
        }
    }
    g_codegen_force_mode = -1;
    std::printf("  random expressions: %d programs checked, %d rejected for size\n", compiled, skipped);
}

int main()
{
    test_preprocess();
    test_errors();
    test_patterns();
    test_vm();
    test_vertex();
    test_regressions();
    test_differential();
    test_random_expressions();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
