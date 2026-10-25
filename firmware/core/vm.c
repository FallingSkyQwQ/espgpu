/*
 * vm.c — 片元 VM 解释器（指令集见 shared/espgpu_vm.h）
 *
 * 逐像素执行，全部 float。没有分支，只有 KIL 能提前结束。
 * 程序在 LOAD_PROGRAM 时经过 gp_vm_validate，执行期不再做越界检查。
 */

#include <math.h>

#include "core_internal.h"

static const float k_zero[4] = {0, 0, 0, 0};
static const float k_one[4] = {1, 1, 1, 1};

static bool reg_ok_src(uint8_t r)
{
    const uint8_t bank = r & 0xC0;
    const uint8_t idx = r & 0x3F;
    switch (bank) {
    case ESPGPU_VM_BANK_TEMP: return idx < ESPGPU_VM_TEMPS;
    case ESPGPU_VM_BANK_VARYING: return idx < ESPGPU_VM_VARYINGS;
    case ESPGPU_VM_BANK_CONST: return idx < ESPGPU_VM_CONSTS;
    default: return idx <= 1;
    }
}

static bool reg_ok_dst(uint8_t r)
{
    return r == ESPGPU_VM_OUT ||
           ((r & 0xC0) == ESPGPU_VM_BANK_TEMP && (r & 0x3F) < ESPGPU_VM_TEMPS);
}

bool gp_vm_validate(const espgpu_vm_insn_t *code, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const espgpu_vm_insn_t *in = &code[i];
        if (in->op >= ESPGPU_VM_OP_COUNT) {
            return false;
        }
        if (in->op == ESPGPU_VM_END || in->op == ESPGPU_VM_NOP) {
            continue;
        }
        for (int k = 0; k < 3; k++) {
            if (!reg_ok_src(in->src[k])) {
                return false;
            }
        }
        if (in->op != ESPGPU_VM_KIL && !reg_ok_dst(in->dst)) {
            return false;
        }
        if (in->op == ESPGPU_VM_TEX && in->tex_unit >= ESPGPU_MAX_TEXTURE_UNITS) {
            return false;
        }
    }
    return true;
}

static inline const float *reg_ptr(uint8_t r, const float *regs, const float *v, const float *k)
{
    const uint8_t idx = r & 0x3F;
    switch (r & 0xC0) {
    case ESPGPU_VM_BANK_TEMP: return regs + idx * 4;
    case ESPGPU_VM_BANK_VARYING: return v + idx * 4;
    case ESPGPU_VM_BANK_CONST: return k ? k + idx * 4 : k_zero;
    default: return idx ? k_one : k_zero;
    }
}

static inline void load_src(const espgpu_vm_insn_t *in, int i, const float *regs, const float *v,
                            const float *k, float o[4])
{
    const float *p = reg_ptr(in->src[i], regs, v, k);
    const uint8_t sw = in->swz[i];
    o[0] = p[sw & 3];
    o[1] = p[(sw >> 2) & 3];
    o[2] = p[(sw >> 4) & 3];
    o[3] = p[(sw >> 6) & 3];
    if (in->neg & (1u << i)) {
        o[0] = -o[0];
        o[1] = -o[1];
        o[2] = -o[2];
        o[3] = -o[3];
    }
}

static inline float frac_f(float x)
{
    return x - floorf(x);
}

GP_HOT bool gp_vm_run(const gp_prog_t *p, const float *consts, const gp_sampler_t *smp,
                      const float *v, float *regs, float out[4])
{
    float o[4] = {0, 0, 0, 1};

    for (uint32_t pc = 0; pc < p->count; pc++) {
        const espgpu_vm_insn_t *in = &p->code[pc];
        const uint8_t op = in->op;
        if (op == ESPGPU_VM_END) {
            break;
        }
        if (op == ESPGPU_VM_NOP) {
            continue;
        }

        float a[4], b[4], c[4], r[4];
        load_src(in, 0, regs, v, consts, a);
        if (op != ESPGPU_VM_MOV && op != ESPGPU_VM_TEX && op != ESPGPU_VM_KIL) {
            load_src(in, 1, regs, v, consts, b);
            load_src(in, 2, regs, v, consts, c);
        } else {
            b[0] = b[1] = b[2] = b[3] = 0.0f;
            c[0] = c[1] = c[2] = c[3] = 0.0f;
        }

        switch (op) {
        case ESPGPU_VM_MOV: for (int i = 0; i < 4; i++) r[i] = a[i]; break;
        case ESPGPU_VM_ADD: for (int i = 0; i < 4; i++) r[i] = a[i] + b[i]; break;
        case ESPGPU_VM_SUB: for (int i = 0; i < 4; i++) r[i] = a[i] - b[i]; break;
        case ESPGPU_VM_MUL: for (int i = 0; i < 4; i++) r[i] = a[i] * b[i]; break;
        case ESPGPU_VM_MAD: for (int i = 0; i < 4; i++) r[i] = a[i] * b[i] + c[i]; break;
        case ESPGPU_VM_DIV:
            for (int i = 0; i < 4; i++) r[i] = b[i] != 0.0f ? a[i] / b[i] : 0.0f;
            break;
        case ESPGPU_VM_DP2: r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1]; break;
        case ESPGPU_VM_DP3:
            r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            break;
        case ESPGPU_VM_DP4:
            r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
            break;
        case ESPGPU_VM_MIN: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? a[i] : b[i]; break;
        case ESPGPU_VM_MAX: for (int i = 0; i < 4; i++) r[i] = a[i] > b[i] ? a[i] : b[i]; break;
        case ESPGPU_VM_RCP: {
            const float x = a[0] != 0.0f ? 1.0f / a[0] : 0.0f;
            r[0] = r[1] = r[2] = r[3] = x;
            break;
        }
        case ESPGPU_VM_RSQ: {
            const float s = fabsf(a[0]);
            const float x = s > 0.0f ? 1.0f / sqrtf(s) : 0.0f;
            r[0] = r[1] = r[2] = r[3] = x;
            break;
        }
        case ESPGPU_VM_SQRT: for (int i = 0; i < 4; i++) r[i] = sqrtf(fabsf(a[i])); break;
        case ESPGPU_VM_EXP2: for (int i = 0; i < 4; i++) r[i] = exp2f(a[i]); break;
        case ESPGPU_VM_LOG2:
            for (int i = 0; i < 4; i++) r[i] = a[i] > 0.0f ? log2f(a[i]) : -126.0f;
            break;
        case ESPGPU_VM_POW:
            for (int i = 0; i < 4; i++) r[i] = powf(a[i] < 0.0f ? 0.0f : a[i], b[i]);
            break;
        case ESPGPU_VM_FRC: for (int i = 0; i < 4; i++) r[i] = frac_f(a[i]); break;
        case ESPGPU_VM_FLR: for (int i = 0; i < 4; i++) r[i] = floorf(a[i]); break;
        case ESPGPU_VM_ABS: for (int i = 0; i < 4; i++) r[i] = fabsf(a[i]); break;
        case ESPGPU_VM_SLT: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
        case ESPGPU_VM_SGE: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
        case ESPGPU_VM_SEQ: for (int i = 0; i < 4; i++) r[i] = a[i] == b[i] ? 1.0f : 0.0f; break;
        case ESPGPU_VM_SNE: for (int i = 0; i < 4; i++) r[i] = a[i] != b[i] ? 1.0f : 0.0f; break;
        case ESPGPU_VM_LRP:
            for (int i = 0; i < 4; i++) r[i] = b[i] + a[i] * (c[i] - b[i]);
            break;
        case ESPGPU_VM_CMP: for (int i = 0; i < 4; i++) r[i] = a[i] < 0.0f ? b[i] : c[i]; break;
        case ESPGPU_VM_SEL: for (int i = 0; i < 4; i++) r[i] = a[i] != 0.0f ? b[i] : c[i]; break;
        case ESPGPU_VM_CLAMP:
            for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? b[i] : (a[i] > c[i] ? c[i] : a[i]);
            break;
        case ESPGPU_VM_SIN: for (int i = 0; i < 4; i++) r[i] = sinf(a[i]); break;
        case ESPGPU_VM_COS: for (int i = 0; i < 4; i++) r[i] = cosf(a[i]); break;
        case ESPGPU_VM_TEX: {
            const gp_sampler_t *s = &smp[in->tex_unit];
            if (s->tex == NULL) {
                r[0] = r[1] = r[2] = r[3] = 1.0f;
            } else {
                const uint32_t px = gp_sample(s, a[0] * (float)s->tex->w, a[1] * (float)s->tex->h);
                r[0] = (float)((px >> 16) & 255) * (1.0f / 255.0f);
                r[1] = (float)((px >> 8) & 255) * (1.0f / 255.0f);
                r[2] = (float)(px & 255) * (1.0f / 255.0f);
                r[3] = (float)(px >> 24) * (1.0f / 255.0f);
            }
            break;
        }
        case ESPGPU_VM_KIL:
            if (a[0] < 0.0f || a[1] < 0.0f || a[2] < 0.0f || a[3] < 0.0f) {
                return false;
            }
            continue;
        case ESPGPU_VM_MOD:
            for (int i = 0; i < 4; i++) {
                r[i] = b[i] != 0.0f ? a[i] - b[i] * floorf(a[i] / b[i]) : 0.0f;
            }
            break;
        case ESPGPU_VM_SIGN:
            for (int i = 0; i < 4; i++) r[i] = a[i] > 0.0f ? 1.0f : (a[i] < 0.0f ? -1.0f : 0.0f);
            break;
        case ESPGPU_VM_STEP: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
        case ESPGPU_VM_SMOOTH:
            for (int i = 0; i < 4; i++) {
                const float d = b[i] - a[i];
                float t = d != 0.0f ? (c[i] - a[i]) / d : (c[i] >= b[i] ? 1.0f : 0.0f);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                r[i] = t * t * (3.0f - 2.0f * t);
            }
            break;
        default:
            continue;
        }

        if (in->flags & ESPGPU_VM_F_SAT) {
            for (int i = 0; i < 4; i++) r[i] = r[i] < 0.0f ? 0.0f : (r[i] > 1.0f ? 1.0f : r[i]);
        }
        float *d = in->dst == ESPGPU_VM_OUT ? o : regs + (in->dst & 0x3F) * 4;
        const uint8_t m = in->mask ? in->mask : 0xF;
        if (m & 1) d[0] = r[0];
        if (m & 2) d[1] = r[1];
        if (m & 4) d[2] = r[2];
        if (m & 8) d[3] = r[3];
    }

    out[0] = o[0];
    out[1] = o[1];
    out[2] = o[2];
    out[3] = o[3];
    return true;
}
