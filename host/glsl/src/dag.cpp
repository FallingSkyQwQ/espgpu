/*
 * dag.cpp — 运算语义、DAG 解释执行（顶点着色器）、VM 参考解释器、反汇编
 *
 * eval_op 是唯一的数值语义来源：编译期常量折叠、主机端顶点着色器执行、
 * VM 参考解释器都调用它，保证三者结果一致。
 */

#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace espgpu::glsl {

void eval_op(uint8_t op, const float a[4], const float b[4], const float c[4], float d[4])
{
    switch (op) {
    case ESPGPU_VM_DP2:
    case ESPGPU_VM_DP3:
    case ESPGPU_VM_DP4: {
        const int n = op == ESPGPU_VM_DP2 ? 2 : op == ESPGPU_VM_DP3 ? 3 : 4;
        float s = 0;
        for (int i = 0; i < n; i++) s += a[i] * b[i];
        d[0] = d[1] = d[2] = d[3] = s;
        return;
    }
    case ESPGPU_VM_RCP: {
        const float v = 1.0f / a[0];
        d[0] = d[1] = d[2] = d[3] = v;
        return;
    }
    case ESPGPU_VM_RSQ: {
        const float v = 1.0f / std::sqrt(std::fabs(a[0]));
        d[0] = d[1] = d[2] = d[3] = v;
        return;
    }
    default: break;
    }
    for (int i = 0; i < 4; i++) {
        const float x = a[i], y = b[i], z = c[i];
        float r = 0;
        switch (op) {
        case ESPGPU_VM_MOV: r = x; break;
        case ESPGPU_VM_ADD: r = x + y; break;
        case ESPGPU_VM_SUB: r = x - y; break;
        case ESPGPU_VM_MUL: r = x * y; break;
        case ESPGPU_VM_MAD: r = x * y + z; break;
        case ESPGPU_VM_DIV: r = x / y; break;
        case ESPGPU_VM_MIN: r = std::min(x, y); break;
        case ESPGPU_VM_MAX: r = std::max(x, y); break;
        case ESPGPU_VM_SQRT: r = std::sqrt(x); break;
        case ESPGPU_VM_EXP2: r = std::exp2(x); break;
        case ESPGPU_VM_LOG2: r = std::log2(x); break;
        case ESPGPU_VM_POW: r = std::pow(x, y); break;
        case ESPGPU_VM_FRC: r = x - std::floor(x); break;
        case ESPGPU_VM_FLR: r = std::floor(x); break;
        case ESPGPU_VM_ABS: r = std::fabs(x); break;
        case ESPGPU_VM_SLT: r = x < y ? 1.0f : 0.0f; break;
        case ESPGPU_VM_SGE: r = x >= y ? 1.0f : 0.0f; break;
        case ESPGPU_VM_SEQ: r = x == y ? 1.0f : 0.0f; break;
        case ESPGPU_VM_SNE: r = x != y ? 1.0f : 0.0f; break;
        case ESPGPU_VM_LRP: r = y + x * (z - y); break;
        case ESPGPU_VM_CMP: r = x < 0 ? y : z; break;
        case ESPGPU_VM_SEL: r = x != 0 ? y : z; break;
        case ESPGPU_VM_CLAMP: r = std::min(std::max(x, y), z); break;
        case ESPGPU_VM_SIN: r = std::sin(x); break;
        case ESPGPU_VM_COS: r = std::cos(x); break;
        case ESPGPU_VM_MOD: r = x - y * std::floor(x / y); break;
        case ESPGPU_VM_SIGN: r = x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f); break;
        case ESPGPU_VM_STEP: r = x >= y ? 1.0f : 0.0f; break;
        case ESPGPU_VM_SMOOTH: {
            float t = (z - x) / (y - x);
            t = std::min(std::max(t, 0.0f), 1.0f);
            r = t * t * (3.0f - 2.0f * t);
            break;
        }
        default: r = 0; break;
        }
        d[i] = r;
    }
}

/* ── 顶点着色器解释执行 ─────────────────────────────────────────── */

namespace {

void gather(const Operand &o, const std::vector<std::array<float, 4>> &val, float out[4])
{
    for (int i = 0; i < 4; i++) {
        const Scalar &s = o.s[std::min<int>(i, o.n - 1)];
        out[i] = val[static_cast<size_t>(s.node)][s.comp];
    }
}

} // namespace

void run_vertex(const Shader &vs, const float (*inputs)[4], const float (*uniforms)[4],
                float out_pos[4], float (*out_var)[4])
{
    const Dag &d = *vs.dag;
    thread_local std::vector<std::array<float, 4>> val;
    if (val.size() < d.nodes.size()) val.resize(d.nodes.size());

    for (int id : d.live_order) {
        const Node &n = d.nodes[static_cast<size_t>(id)];
        std::array<float, 4> &r = val[static_cast<size_t>(id)];
        switch (n.kind) {
        case NK::Const: r = {n.c, n.c, n.c, n.c}; break;
        case NK::Attrib:
            for (int i = 0; i < 4; i++) r[static_cast<size_t>(i)] = inputs[n.index][i];
            break;
        case NK::Uniform:
            for (int i = 0; i < 4; i++) r[static_cast<size_t>(i)] = uniforms[n.index][i];
            break;
        case NK::Op: {
            float a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0}, c[4] = {0, 0, 0, 0};
            if (n.nargs > 0) gather(n.a[0], val, a);
            if (n.nargs > 1) gather(n.a[1], val, b);
            if (n.nargs > 2) gather(n.a[2], val, c);
            eval_op(n.op, a, b, c, r.data());
            break;
        }
        default: r = {0, 0, 0, 0}; break;
        }
    }

    auto fetch = [&](const Operand &o, float *dst, int n) {
        for (int i = 0; i < n; i++) {
            if (i < o.n) {
                const Scalar &s = o.s[i];
                dst[i] = val[static_cast<size_t>(s.node)][s.comp];
            } else {
                dst[i] = i == 3 ? 1.0f : 0.0f;
            }
        }
    };
    fetch(d.position, out_pos, 4);
    for (size_t j = 0; j < d.var_out.size(); j++) {
        fetch(d.var_out[j], out_var[j], 4);
        for (int i = d.var_out[j].n; i < 4; i++) out_var[j][i] = i == 3 ? 1.0f : 0.0f;
    }
}

/* ── VM 参考解释器 ─────────────────────────────────────────────── */

bool vm_run(const espgpu_vm_insn_t *code, size_t n, const float v[ESPGPU_VM_VARYINGS][4],
            const float c[ESPGPU_VM_CONSTS][4], TexFn tex, void *user, float out[4])
{
    /* 与设备一致：临时寄存器在像素之间不清零（这里填 NaN，读到未写的寄存器会立刻暴露），
     * o0 初值 (0,0,0,1)，mask == 0 等于写全部四个分量 */
    float r[ESPGPU_VM_TEMPS][4];
    for (auto &t : r) t[0] = t[1] = t[2] = t[3] = std::numeric_limits<float>::quiet_NaN();
    float o[4] = {0, 0, 0, 1};

    auto src = [&](const espgpu_vm_insn_t &in, int k, float dst[4]) {
        const uint8_t ref = in.src[k];
        const int idx = ref & 0x3F;
        const float *base;
        static const float zero[4] = {0, 0, 0, 0};
        static const float one[4] = {1, 1, 1, 1};
        switch (ref & 0xC0) {
        case ESPGPU_VM_BANK_TEMP: base = idx < static_cast<int>(ESPGPU_VM_TEMPS) ? r[idx] : zero; break;
        case ESPGPU_VM_BANK_VARYING: base = idx < static_cast<int>(ESPGPU_VM_VARYINGS) ? v[idx] : zero; break;
        case ESPGPU_VM_BANK_CONST: base = idx < static_cast<int>(ESPGPU_VM_CONSTS) ? c[idx] : zero; break;
        default: base = idx == 1 ? one : zero; break;
        }
        const uint8_t sw = in.swz[k];
        for (int i = 0; i < 4; i++) {
            float x = base[(sw >> (i * 2)) & 3];
            if (in.neg & (1u << k)) x = -x;
            dst[i] = x;
        }
    };

    for (size_t pc = 0; pc < n; pc++) {
        const espgpu_vm_insn_t &in = code[pc];
        if (in.op == ESPGPU_VM_END) break;
        if (in.op == ESPGPU_VM_NOP) continue;
        float a[4], b[4], cc[4], d[4] = {0, 0, 0, 0};
        src(in, 0, a);
        src(in, 1, b);
        src(in, 2, cc);
        if (in.op == ESPGPU_VM_KIL) {
            if (a[0] < 0 || a[1] < 0 || a[2] < 0 || a[3] < 0) return false;
            continue;
        }
        if (in.op == ESPGPU_VM_TEX) {
            if (tex) tex(in.tex_unit, a[0], a[1], d, user);
            else d[0] = d[1] = d[2] = d[3] = 1.0f;
        } else {
            eval_op(in.op, a, b, cc, d);
        }
        if (in.flags & ESPGPU_VM_F_SAT) {
            for (float &x : d) x = std::min(std::max(x, 0.0f), 1.0f);
        }
        float *dst;
        if ((in.dst & 0xC0) == ESPGPU_VM_BANK_SPECIAL) {
            dst = o;
        } else {
            const int idx = in.dst & 0x3F;
            if (idx >= static_cast<int>(ESPGPU_VM_TEMPS)) continue;
            dst = r[idx];
        }
        const uint8_t mask = in.mask ? in.mask : 0xF;
        for (int i = 0; i < 4; i++) {
            if (mask & (1u << i)) dst[i] = d[i];
        }
    }
    for (int i = 0; i < 4; i++) out[i] = o[i];
    return true;
}

std::string vm_disassemble(const espgpu_vm_insn_t *code, size_t n)
{
    static const char comp[] = "xyzw";
    auto reg = [](uint8_t ref, bool dst) {
        const int idx = ref & 0x3F;
        char buf[16];
        switch (ref & 0xC0) {
        case ESPGPU_VM_BANK_TEMP: std::snprintf(buf, sizeof(buf), "r%d", idx); break;
        case ESPGPU_VM_BANK_VARYING: std::snprintf(buf, sizeof(buf), "v%d", idx); break;
        case ESPGPU_VM_BANK_CONST: std::snprintf(buf, sizeof(buf), "c%d", idx); break;
        default: std::snprintf(buf, sizeof(buf), "%s", dst ? "o0" : (idx == 1 ? "one" : "zero")); break;
        }
        return std::string(buf);
    };
    auto nsrc = [](uint8_t op) {
        switch (op) {
        case ESPGPU_VM_MAD: case ESPGPU_VM_LRP: case ESPGPU_VM_CMP: case ESPGPU_VM_SEL:
        case ESPGPU_VM_CLAMP: case ESPGPU_VM_SMOOTH: return 3;
        case ESPGPU_VM_MOV: case ESPGPU_VM_RCP: case ESPGPU_VM_RSQ: case ESPGPU_VM_SQRT:
        case ESPGPU_VM_EXP2: case ESPGPU_VM_LOG2: case ESPGPU_VM_FRC: case ESPGPU_VM_FLR:
        case ESPGPU_VM_ABS: case ESPGPU_VM_SIN: case ESPGPU_VM_COS: case ESPGPU_VM_TEX:
        case ESPGPU_VM_KIL: case ESPGPU_VM_SIGN: return 1;
        case ESPGPU_VM_NOP: case ESPGPU_VM_END: return 0;
        default: return 2;
        }
    };
    std::string s;
    for (size_t pc = 0; pc < n; pc++) {
        const espgpu_vm_insn_t &in = code[pc];
        char line[160];
        int k = std::snprintf(line, sizeof(line), "%3zu  %s%s", pc, espgpu_vm_op_name(in.op),
                              (in.flags & ESPGPU_VM_F_SAT) ? "_sat" : "");
        const int ns = nsrc(in.op);
        bool first = true;
        if (in.op != ESPGPU_VM_KIL && in.op != ESPGPU_VM_END && in.op != ESPGPU_VM_NOP) {
            std::string m;
            for (int i = 0; i < 4; i++) if (in.mask & (1u << i)) m += comp[i];
            k += std::snprintf(line + k, sizeof(line) - static_cast<size_t>(k), " %s.%s", reg(in.dst, true).c_str(), m.c_str());
            first = false;
        }
        for (int j = 0; j < ns; j++) {
            std::string sw;
            for (int i = 0; i < 4; i++) sw += comp[(in.swz[j] >> (i * 2)) & 3];
            k += std::snprintf(line + k, sizeof(line) - static_cast<size_t>(k), "%s %s%s.%s", first ? "" : ",",
                               (in.neg & (1u << j)) ? "-" : "", reg(in.src[j], false).c_str(), sw.c_str());
            first = false;
        }
        if (in.op == ESPGPU_VM_TEX) {
            std::snprintf(line + k, sizeof(line) - static_cast<size_t>(k), ", unit%u", in.tex_unit);
        }
        s += line;
        s += '\n';
    }
    return s;
}

} // namespace espgpu::glsl
