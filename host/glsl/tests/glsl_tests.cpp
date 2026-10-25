/*
 * glsl_tests.cpp — 编译器单元测试（平台无关）
 *
 * 片元着色器用 VM 参考解释器执行，结果与手算的期望值比较。
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "espgpu_glsl.h"
#include "espgpu_proto.h"

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
    for (const auto &u : s.uniforms) {
        auto it = e.uniforms.find(u.name);
        if (it == e.uniforms.end() || u.local_slot < 0) continue;
        for (size_t k = 0; k < it->second.size(); k++) c[u.local_slot + static_cast<int>(k / 4)][k % 4] = it->second[k];
    }
    for (const auto &cs : s.frag.consts) {
        for (int k = 0; k < 4; k++) c[cs.slot][k] = cs.v[static_cast<size_t>(k)];
    }
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
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_TEX_MOD_COLOR && s.frag.color_uniform_slot == 0);
    }
    {
        /* 常量乘 1 被化简掉，仍然是模式 */
        Shader s = fs(std::string(kPre300) + "in vec4 c;\nout vec4 o;\nvoid main(){ o = c * 1.0; }");
        CHECK(s.ok && !s.frag.program && s.frag.shade_mode == ESPGPU_SHADE_FLAT);
    }
    {
        Shader s = fs(std::string(kPre300) + "in vec4 c;\nout vec4 o;\nvoid main(){ o = c * 0.5; }");
        CHECK(s.ok && s.frag.program);
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
        e.uniforms["@fragcoord"] = {1, 1, 0, 1};
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
        e.uniforms["@fragcoord"] = {2, 2, 200, -1}; /* 设备 320x200，窗口 640x400 */
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

int main()
{
    test_preprocess();
    test_errors();
    test_patterns();
    test_vm();
    test_vertex();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
