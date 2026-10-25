/*
 * espgpu_vm_math.h — 片元 VM 的标量数学语义（设备 VM、主机参考 VM、常量折叠、主机顶点着色器共用）
 *
 * 为什么不用 libm：
 *   1. 速度：ESP32-S3（Xtensa LX7，单精度 FPU）上 newlib 的 sinf / exp2f / powf 是几百周期的库调用，
 *      floorf 也是库调用；plasma 每像素 20 次 sinf，320x200 一帧 1.4 s（issue #2）。这里全是加 / 乘 / madd、
 *      float↔int 转换和整数位运算，热路径没有任何函数调用。
 *   2. 一致性：主机（MSVC CRT）和设备（newlib）的 sinf 结果不同；共用同一份多项式后，
 *      同一编译器 / 同一 FP 收缩设置下逐位相同。设备 GCC 默认 -ffp-contract=fast 会把 a*b+c
 *      合成 MADD.S（单次舍入），主机 x86 不会，所以跨平台可能差 1~2 ulp（远小于下面的误差上限）。
 *
 * DIV / RCP / SQRT / RSQ 仍用 IEEE 运算：它们本身就是正确舍入、跨平台逐位一致的，近似版本
 * 反而会破坏 floor(6.0 / 3.0) == 2.0、sqrt(4.0) == 2.0 这类着色器常依赖的精确性。
 *
 * 精度（对 libm 双精度真值，tests/glsl_tests.cpp 里稠密扫描验证）：
 *   sin / cos   绝对误差 ≤ 1.3e-7（|x| ≤ 1e4），任意有限输入 ≤ 1e-6。|x| ≤ ESPGPU_VM_TRIG_FAST（≈2.05e5）走快速路径
 *               （Cody–Waite 三段 π/2 约简，周期性保持到这个范围）；更大的 |x| 走冷路径
 *               （Payne–Hanek，用 1/π 的 256 位常数精确约简），结果仍然准确。
 *   exp2        相对误差 ≤ 2e-7（结果为正规数时）
 *   log2        绝对误差 ≤ 2e-7·max(1,|log2 x|)，x 接近 1 时相对误差 ≤ 3e-7
 *   pow         exp2(b·log2 a)，相对误差约 (1 + |b·log2 a|)·2e-7
 *   floor/fract 精确
 *
 * 边界语义（所有实现必须一致，改这里就是改 ISA）：
 *   sin/cos(NaN 或 ±inf)  = NaN
 *   exp2(NaN) = NaN；exp2(x ≥ 128) = +inf；exp2(x < -126) = 0（不产生非正规数）
 *   log2(x) = -126，当 x < 2^-126（含 0、负数、非正规数、-inf）或 x 为 NaN；log2(+inf) = +inf
 *   pow(a, b)：b == 0 或 a == 1 → 1；a ≤ 0 或 NaN（按 0 处理）→ b > 0 ? 0 : b < 0 ? +inf : NaN；
 *              其余 = exp2(b·log2 a)
 *   floor(x)：|x| ≥ 2^23、±inf、NaN 原样返回；floor(-0) = +0
 *   fract(x) = x - floor(x)（fract(±inf) = NaN）
 *   mod(a, b) = b == 0 ? 0 : a - b·floor(a / b)
 *   a / b = b == 0 ? 0 : a / b；rcp(a) = a == 0 ? 0 : 1 / a
 *   sqrt(a) = sqrt(|a|)；rsq(a) = |a| > 0 ? 1 / sqrt(|a|) : 0
 *   smoothstep(a, b, x)：b == a 时 x >= b ? 1 : 0
 *   _sat：夹到 [0,1]，NaN 保持 NaN
 *
 * 纯 C99 / C++ 头文件，C 与 C++ 都能包含。
 */

#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "espgpu_vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 包含前定义 ESPGPU_VM_FORCE_INLINE（固件 vm.c）可强制内联：热路径里不留不在 IRAM 的副本 */
#if defined(ESPGPU_VM_FORCE_INLINE) && (defined(__GNUC__) || defined(__clang__))
#define ESPGPU_VM_FN static inline __attribute__((always_inline))
#else
#define ESPGPU_VM_FN static inline
#endif

#if defined(_MSC_VER)
#define ESPGPU_VM_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define ESPGPU_VM_NOINLINE __attribute__((noinline))
#else
#define ESPGPU_VM_NOINLINE
#endif

/** 快速三角路径的输入范围：|x| ≤ 65535·π 时 m = 2n(+1) ≤ 2^17，m·Q1、m·Q2 都是精确乘积 */
#define ESPGPU_VM_TRIG_FAST 205000.0f

ESPGPU_VM_FN uint32_t espgpu_vm_f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

ESPGPU_VM_FN float espgpu_vm_u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

ESPGPU_VM_FN float espgpu_vm_inf(void)
{
    return espgpu_vm_u2f(0x7F800000u);
}

ESPGPU_VM_FN float espgpu_vm_nan(void)
{
    return espgpu_vm_u2f(0x7FC00000u);
}

/* ── sin / cos ─────────────────────────────────────────────────────── */

/* π/2 = Q1 + Q2 + Q3：Q1、Q2 各 7 个有效位（与 ≤17 位的 m 相乘精确），Q3 为余项（再余 2.6e-12） */
#define ESPGPU_VM_Q1 1.5625f
#define ESPGPU_VM_Q2 0.0081787109375f
#define ESPGPU_VM_Q3 1.17615854833275e-4f
#define ESPGPU_VM_INV_PI 0.318309886183791f
#define ESPGPU_VM_PI 3.14159265358979f
/* sin r ≈ r + r^3 (S1 + S2 z + S3 z^2 + S4 z^3)，z = r^2，|r| ≤ π/2 上 minimax（z 变量拟合）：
 * float 求值误差 1.1e-7；sin(±π/2) 恰为 ±1（cos(0) == 1）。用 FMA 求值时 π/2 附近可能比 1 大 1 ulp
 * （|结果| ≤ 1 + 1.2e-7）——同时保证这两条做不到，夹取要多 4 条指令，不值得 */
#define ESPGPU_VM_S1 -1.666666120e-01f
#define ESPGPU_VM_S2 8.333084174e-03f
#define ESPGPU_VM_S3 -1.980995585e-04f
#define ESPGPU_VM_S4 2.605166173e-06f

ESPGPU_VM_FN float espgpu_vm_sin_poly(float r)
{
    const float z = r * r;
    float p = ESPGPU_VM_S3 + z * ESPGPU_VM_S4;
    p = ESPGPU_VM_S2 + z * p;
    p = ESPGPU_VM_S1 + z * p;
    return r + (r * z) * p;
}

/**
 * 冷路径：|x| > ESPGPU_VM_TRIG_FAST 或非有限。Payne–Hanek：x = M·2^E（M 为 24 位整数），
 * 取 1/π 从第 E-1 位起的 64 位窗口 W，则 (M·W) mod 2^64 = (x/π mod 4)·2^62（更高的位贡献 4 的整数倍）。
 * 截断误差 < M·2^-62 ≤ 2^-38 周期。cos 用 x/π - 1/2，与快速路径同一套奇偶规则。
 */
static ESPGPU_VM_NOINLINE float espgpu_vm_trig_big(float x, int is_cos)
{
    static const uint32_t k_inv_pi[9] = {
        0u, /* 第 -31..0 位：1/π < 1，全 0 */
        0x517CC1B7u, 0x27220A94u, 0xFE13ABE8u, 0xFA9A6EE0u,
        0x6DB14ACCu, 0x9E21C820u, 0xFF28B1D5u, 0xEF5DE2B0u,
    };
    const uint32_t u = espgpu_vm_f2u(x);
    const uint32_t ex = (u >> 23) & 0xFFu;
    if (ex == 0xFFu) {
        return espgpu_vm_nan(); /* ±inf、NaN */
    }
    if (ex < 127u) {
        return espgpu_vm_nan(); /* 前置条件 |x| ≥ 1（快速路径只把 |x| > 2e5 交过来）；保护查表下标 */
    }
    const uint64_t m = (u & 0x7FFFFFu) | 0x800000u;
    const int e = (int)ex - 150;  /* |x| = m · 2^e，e ≥ -23 */
    const int s = e - 1 + 31;     /* 窗口首位（1/π 第 e-1 位）在表里的位下标，≥ 7 */
    const int q = s >> 5, sh = s & 31;
    uint64_t w = ((uint64_t)k_inv_pi[q] << 32) | k_inv_pi[q + 1];
    if (sh) {
        w = (w << sh) | (k_inv_pi[q + 2] >> (32 - sh));
    }
    /* y = (|x|/π mod 4)·2^62；sin 取 n = round(t)，cos 取 n = floor(t) = round(t - 1/2) */
    uint64_t y = m * w;
    if (!is_cos) {
        y += (uint64_t)1 << 61;
    }
    const uint32_t n_odd = (uint32_t)(y >> 62) & 1u;
    /* g = 4·((y mod 2^62) - 2^61)：带符号的余数，高 32 位 = (t - n [- 1/2])·2^32 */
    const uint64_t g = (y << 2) - ((uint64_t)1 << 63);
    const int32_t f = (int32_t)(uint32_t)(g >> 32);
    const float r = (float)f * (ESPGPU_VM_PI / 4294967296.0f);
    float v = espgpu_vm_sin_poly(r);
    /* sin(|x|) = (-1)^n sin r；cos(|x|) = (-1)^(n+1) sin r */
    if (n_odd ^ (uint32_t)(is_cos != 0)) {
        v = -v;
    }
    if (!is_cos && (u >> 31)) {
        v = -v; /* sin 是奇函数 */
    }
    return v;
}

ESPGPU_VM_FN float espgpu_vm_sin(float x)
{
    if (!(fabsf(x) <= ESPGPU_VM_TRIG_FAST)) {
        return espgpu_vm_trig_big(x, 0);
    }
    /* n = round(x/π)：加 1.5·2^23 后尾数最低位就是 n 的奇偶 */
    const float t = x * ESPGPU_VM_INV_PI + 12582912.0f;
    const uint32_t odd = espgpu_vm_f2u(t) & 1u;
    const float n = t - 12582912.0f;
    const float m = n + n;
    float r = x - m * ESPGPU_VM_Q1;
    r = r - m * ESPGPU_VM_Q2;
    r = r - m * ESPGPU_VM_Q3;
    const float v = espgpu_vm_sin_poly(r);
    return odd ? -v : v;
}

ESPGPU_VM_FN float espgpu_vm_cos(float x)
{
    if (!(fabsf(x) <= ESPGPU_VM_TRIG_FAST)) {
        return espgpu_vm_trig_big(x, 1);
    }
    /* x = (n + 1/2)π + r → cos x = (-1)^(n+1) sin r */
    const float t = (x * ESPGPU_VM_INV_PI - 0.5f) + 12582912.0f;
    const uint32_t odd = espgpu_vm_f2u(t) & 1u;
    const float n = t - 12582912.0f;
    const float m = n + n + 1.0f;
    float r = x - m * ESPGPU_VM_Q1;
    r = r - m * ESPGPU_VM_Q2;
    r = r - m * ESPGPU_VM_Q3;
    const float v = espgpu_vm_sin_poly(r);
    return odd ? v : -v;
}

/* ── floor / fract / mod ───────────────────────────────────────────── */

ESPGPU_VM_FN float espgpu_vm_floor(float x)
{
    if (!(fabsf(x) < 8388608.0f)) {
        return x; /* |x| ≥ 2^23 已是整数；±inf / NaN 原样 */
    }
    const float f = (float)(int32_t)x; /* 向零取整，|x| < 2^23 不会溢出 */
    return f > x ? f - 1.0f : f;
}

ESPGPU_VM_FN float espgpu_vm_fract(float x)
{
    return x - espgpu_vm_floor(x);
}

ESPGPU_VM_FN float espgpu_vm_div(float a, float b)
{
    return b != 0.0f ? a / b : 0.0f;
}

ESPGPU_VM_FN float espgpu_vm_mod(float a, float b)
{
    return b != 0.0f ? a - b * espgpu_vm_floor(a / b) : 0.0f;
}

/* ── exp2 / log2 / pow ─────────────────────────────────────────────── */

ESPGPU_VM_FN float espgpu_vm_exp2(float x)
{
    if (!(x < 128.0f)) {
        return x != x ? x : espgpu_vm_inf();
    }
    if (!(x >= -126.0f)) {
        return 0.0f;
    }
    int32_t i = (int32_t)x;
    if ((float)i > x) {
        i--;
    }
    const float f = x - (float)i; /* [0,1) */
    /* 2^f ≈ 1 + f·P(f)，[0,1) 上相对误差 minimax 8e-8 */
    float p = 1.867130050e-03f;
    p = 9.017030708e-03f + f * p;
    p = 5.579991266e-02f + f * p;
    p = 2.401644439e-01f + f * p;
    p = 6.931512952e-01f + f * p;
    p = 1.0f + f * p;
    /* p ∈ [1,2]，i ∈ [-126,127]：直接加到指数上，结果总是正规数（或溢出成 inf） */
    return espgpu_vm_u2f(espgpu_vm_f2u(p) + ((uint32_t)i << 23));
}

ESPGPU_VM_FN float espgpu_vm_log2(float x)
{
    if (!(x >= 1.17549435e-38f)) {
        return -126.0f; /* ≤ 0、非正规数、NaN */
    }
    uint32_t u = espgpu_vm_f2u(x);
    if (u >= 0x7F800000u) {
        return x; /* +inf */
    }
    int32_t e = (int32_t)(u >> 23) - 127;
    u = (u & 0x7FFFFFu) | 0x3F800000u; /* m ∈ [1,2) */
    if (u >= 0x3FB504F3u) {            /* m ≥ √2 → m/2 ∈ [√½,1)，e+1 */
        u -= 0x800000u;
        e++;
    }
    const float t = espgpu_vm_u2f(u) - 1.0f; /* [√½-1, √2-1)，精确 */
    /* log2(1+t) ≈ t·R(t)，相对误差 minimax 1.7e-7 */
    float p = -1.458116621e-01f;
    p = 2.340423614e-01f + t * p;
    p = -2.488768697e-01f + t * p;
    p = 2.870987058e-01f + t * p;
    p = -3.602396250e-01f + t * p;
    p = 4.809232354e-01f + t * p;
    p = -7.213528156e-01f + t * p;
    p = 1.442694902e+00f + t * p;
    return t * p + (float)e;
}

ESPGPU_VM_FN float espgpu_vm_pow(float a, float b)
{
    if (b == 0.0f || a == 1.0f) {
        return 1.0f;
    }
    if (!(a > 0.0f)) {
        return b > 0.0f ? 0.0f : (b < 0.0f ? espgpu_vm_inf() : b);
    }
    return espgpu_vm_exp2(b * espgpu_vm_log2(a));
}

/* ── 其它（与 libm 无关，但语义也在这里统一） ─────────────────────── */

ESPGPU_VM_FN float espgpu_vm_rcp(float a)
{
    return a != 0.0f ? 1.0f / a : 0.0f;
}

ESPGPU_VM_FN float espgpu_vm_rsq(float a)
{
    const float s = fabsf(a);
    return s > 0.0f ? 1.0f / sqrtf(s) : 0.0f;
}

ESPGPU_VM_FN float espgpu_vm_sqrt(float a)
{
    return sqrtf(fabsf(a));
}

ESPGPU_VM_FN float espgpu_vm_sat(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

ESPGPU_VM_FN float espgpu_vm_sign(float x)
{
    return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f);
}

ESPGPU_VM_FN float espgpu_vm_lrp(float t, float a, float b)
{
    return a + t * (b - a);
}

ESPGPU_VM_FN float espgpu_vm_clamp(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

ESPGPU_VM_FN float espgpu_vm_smooth(float a, float b, float x)
{
    const float d = b - a;
    float t = d != 0.0f ? (x - a) / d : (x >= b ? 1.0f : 0.0f);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

/** dot(a, b) 的前 n 个分量：s = a0·b0，再逐个 s = ai·bi + s（与设备上 MUL + MAD 链的舍入一致） */
ESPGPU_VM_FN float espgpu_vm_dot(int n, const float *a, const float *b)
{
    float s = a[0] * b[0];
    for (int i = 1; i < n; i++) {
        s = a[i] * b[i] + s;
    }
    return s;
}

/**
 * 逐分量指令的标量语义（DP2/3/4、TEX、KIL 除外；RCP / RSQ 取 a 即 .x）。
 * 设备 VM、主机参考 VM、常量折叠都调用它；op 为常量时内联后就是一条表达式。
 */
ESPGPU_VM_FN float espgpu_vm_eval_scalar(uint8_t op, float a, float b, float c)
{
    switch (op) {
    case ESPGPU_VM_MOV: return a;
    case ESPGPU_VM_ADD: return a + b;
    case ESPGPU_VM_SUB: return a - b;
    case ESPGPU_VM_MUL: return a * b;
    case ESPGPU_VM_MAD: return a * b + c;
    case ESPGPU_VM_DIV: return espgpu_vm_div(a, b);
    case ESPGPU_VM_MIN: return a < b ? a : b;
    case ESPGPU_VM_MAX: return a > b ? a : b;
    case ESPGPU_VM_RCP: return espgpu_vm_rcp(a);
    case ESPGPU_VM_RSQ: return espgpu_vm_rsq(a);
    case ESPGPU_VM_SQRT: return espgpu_vm_sqrt(a);
    case ESPGPU_VM_EXP2: return espgpu_vm_exp2(a);
    case ESPGPU_VM_LOG2: return espgpu_vm_log2(a);
    case ESPGPU_VM_POW: return espgpu_vm_pow(a, b);
    case ESPGPU_VM_FRC: return espgpu_vm_fract(a);
    case ESPGPU_VM_FLR: return espgpu_vm_floor(a);
    case ESPGPU_VM_ABS: return fabsf(a);
    case ESPGPU_VM_SLT: return a < b ? 1.0f : 0.0f;
    case ESPGPU_VM_SGE: return a >= b ? 1.0f : 0.0f;
    case ESPGPU_VM_SEQ: return a == b ? 1.0f : 0.0f;
    case ESPGPU_VM_SNE: return a != b ? 1.0f : 0.0f;
    case ESPGPU_VM_LRP: return espgpu_vm_lrp(a, b, c);
    case ESPGPU_VM_CMP: return a < 0.0f ? b : c;
    case ESPGPU_VM_SEL: return a != 0.0f ? b : c;
    case ESPGPU_VM_CLAMP: return espgpu_vm_clamp(a, b, c);
    case ESPGPU_VM_SIN: return espgpu_vm_sin(a);
    case ESPGPU_VM_COS: return espgpu_vm_cos(a);
    case ESPGPU_VM_MOD: return espgpu_vm_mod(a, b);
    case ESPGPU_VM_SIGN: return espgpu_vm_sign(a);
    case ESPGPU_VM_STEP: return a >= b ? 1.0f : 0.0f;
    case ESPGPU_VM_SMOOTH: return espgpu_vm_smooth(a, b, c);
    default: return 0.0f;
    }
}

#ifdef __cplusplus
}
#endif
