/*
 * espgpu_vm.h — 片元 VM 指令集（M6）
 *
 * GLSL ES 片元着色器若命中内置着色模式，host 直接折叠成 PIPELINE 状态；
 * 否则编译成这里的字节码，以 ESPGPU_SHADE_PROGRAM 在设备上逐像素解释执行。
 *
 * 模型（与 ARB_fragment_program 相近，刻意没有分支）：
 *   - r0..r15   临时寄存器，vec4 float，每像素开始时清零
 *   - v0..v7    插值后的 varying（透视校正），只读
 *                 v0 = COLOR 属性，v1 = TEXCOORD0，v2 = TEXCOORD1，v3 = TEXCOORD2，
 *                 v4 = TEXCOORD3，v5 = NORMAL，v6 = 片元坐标 (x, y, z, 1/w)，v7 保留
 *   - c0..c31   常量寄存器（SET_UNIFORMS），vec4 float
 *   - o0        输出颜色（写入后经混合 / alpha 测试 / 雾）
 *
 * 编译器负责：if/else → 两侧都算再用 CMP 选择；常量次数循环展开；
 * 函数内联。动态循环不支持（编译期报错）。
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)

#define ESPGPU_VM_TEMPS 16u
#define ESPGPU_VM_VARYINGS 8u
#define ESPGPU_VM_CONSTS 32u
#define ESPGPU_VM_MAX_INSNS 256u
#define ESPGPU_VM_MAX_PROGRAMS 16u

/** 寄存器引用：高 2 位是寄存器组，低 6 位是编号 */
enum {
    ESPGPU_VM_BANK_TEMP = 0x00,
    ESPGPU_VM_BANK_VARYING = 0x40,
    ESPGPU_VM_BANK_CONST = 0x80,
    ESPGPU_VM_BANK_SPECIAL = 0xC0, /**< 目标：0 = o0 输出颜色；源：0 = (0,0,0,0) 1 = (1,1,1,1) */
};

#define ESPGPU_VM_REG(bank, idx) ((uint8_t)((bank) | ((idx) & 0x3F)))
#define ESPGPU_VM_OUT ESPGPU_VM_REG(ESPGPU_VM_BANK_SPECIAL, 0)
#define ESPGPU_VM_ZERO ESPGPU_VM_REG(ESPGPU_VM_BANK_SPECIAL, 0)
#define ESPGPU_VM_ONE ESPGPU_VM_REG(ESPGPU_VM_BANK_SPECIAL, 1)

/** swizzle：每分量 2 位，bit[1:0] = x 取哪个分量……bit[7:6] = w。0xE4 = .xyzw */
#define ESPGPU_VM_SWZ(x, y, z, w) ((uint8_t)((x) | ((y) << 2) | ((z) << 4) | ((w) << 6)))
#define ESPGPU_VM_SWZ_IDENTITY 0xE4u

enum {
    ESPGPU_VM_NOP = 0,
    ESPGPU_VM_MOV,  /**< d = a */
    ESPGPU_VM_ADD,  /**< d = a + b */
    ESPGPU_VM_SUB,  /**< d = a - b */
    ESPGPU_VM_MUL,  /**< d = a * b */
    ESPGPU_VM_MAD,  /**< d = a * b + c */
    ESPGPU_VM_DIV,  /**< d = a / b（分量） */
    ESPGPU_VM_DP2,  /**< d = dot(a.xy, b.xy)（广播） */
    ESPGPU_VM_DP3,
    ESPGPU_VM_DP4,
    ESPGPU_VM_MIN,
    ESPGPU_VM_MAX,
    ESPGPU_VM_RCP,  /**< d = 1 / a.x（广播） */
    ESPGPU_VM_RSQ,  /**< d = 1 / sqrt(|a.x|)（广播） */
    ESPGPU_VM_SQRT, /**< 分量 */
    ESPGPU_VM_EXP2, /**< 分量 */
    ESPGPU_VM_LOG2, /**< 分量 */
    ESPGPU_VM_POW,  /**< 分量 a^b */
    ESPGPU_VM_FRC,  /**< a - floor(a) */
    ESPGPU_VM_FLR,
    ESPGPU_VM_ABS,
    ESPGPU_VM_SLT,  /**< d = a < b ? 1 : 0 */
    ESPGPU_VM_SGE,  /**< d = a >= b ? 1 : 0 */
    ESPGPU_VM_SEQ,  /**< d = a == b ? 1 : 0 */
    ESPGPU_VM_SNE,
    ESPGPU_VM_LRP,  /**< d = mix(b, c, a)  = b + a * (c - b) */
    ESPGPU_VM_CMP,  /**< d = a < 0 ? b : c */
    ESPGPU_VM_SEL,  /**< d = a != 0 ? b : c（分量） */
    ESPGPU_VM_CLAMP,/**< d = clamp(a, b, c) */
    ESPGPU_VM_SIN,  /**< 分量 */
    ESPGPU_VM_COS,
    ESPGPU_VM_TEX,  /**< d = texture(unit, a.xy)，unit = insn.tex_unit */
    ESPGPU_VM_KIL,  /**< 任一分量 a < 0 则丢弃片元 */
    ESPGPU_VM_MOD,  /**< d = a - b * floor(a / b) */
    ESPGPU_VM_SIGN,
    ESPGPU_VM_STEP, /**< d = a >= b ? 1 : 0（GLSL step(edge=b, x=a) 的写法由编译器换序） */
    ESPGPU_VM_SMOOTH, /**< d = smoothstep(a, b, c) */
    ESPGPU_VM_END,
    ESPGPU_VM_OP_COUNT,
};

enum {
    ESPGPU_VM_F_SAT = 1u << 0, /**< 结果夹到 [0,1] */
};

/** 一条指令 12 字节 */
typedef struct {
    uint8_t op;       /**< ESPGPU_VM_* */
    uint8_t dst;      /**< 寄存器引用；只能是 TEMP 或 OUT */
    uint8_t mask;     /**< 写掩码 bit0=x … bit3=w */
    uint8_t flags;    /**< ESPGPU_VM_F_* */
    uint8_t src[3];   /**< 寄存器引用 */
    uint8_t neg;      /**< bit i = src[i] 取负 */
    uint8_t swz[3];   /**< 每个源的 swizzle */
    uint8_t tex_unit; /**< TEX 使用的纹理单元 */
} espgpu_vm_insn_t;

#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(espgpu_vm_insn_t) == 12, "vm insn must be 12 bytes");
#else
_Static_assert(sizeof(espgpu_vm_insn_t) == 12, "vm insn must be 12 bytes");
#endif

/** 指令助记符，供反汇编 / 调试输出 */
static inline const char *espgpu_vm_op_name(uint8_t op)
{
    static const char *const names[ESPGPU_VM_OP_COUNT] = {
        "nop", "mov", "add", "sub", "mul", "mad", "div", "dp2", "dp3", "dp4",
        "min", "max", "rcp", "rsq", "sqrt", "exp2", "log2", "pow", "frc", "flr",
        "abs", "slt", "sge", "seq", "sne", "lrp", "cmp", "sel", "clamp", "sin",
        "cos", "tex", "kil", "mod", "sign", "step", "smooth", "end",
    };
    return op < ESPGPU_VM_OP_COUNT ? names[op] : "???";
}

#ifdef __cplusplus
}
#endif
