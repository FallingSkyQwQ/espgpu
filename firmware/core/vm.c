/*
 * vm.c — 片元 VM 解释器（指令集见 shared/espgpu_vm.h，数值语义见 shared/espgpu_vm_math.h）
 *
 * 程序在 LOAD_PROGRAM 时经过 gp_vm_validate：校验之后顺便"预解码"成标量微指令（uop）：
 *   - 每条向量指令按写掩码拆成逐分量的标量 uop，掩码外的分量根本不算（sin r0.x 只算一次 sin，
 *     原来算 4 次）；DP2/3/4 拆成 MUL + MAD 链，RCP/RSQ 算一次再复制
 *   - 所有操作数都变成一张小标量寄存器表 S[] 里的下标：用到的 temp 分量、o0、常量分量、
 *     varying 分量（取负的常量 / varying 另占一格，取负在每像素拷入时做）、0 / 1 / -1 / -0
 *   - 复制传播 + 死代码删除（编译器拼 vec3 时的 mov、DP 结果的广播复制大多会消失）
 *   - 只有"先读后写"的 temp 分量才每像素清零（规范：r0..r15 每像素开始为 0）
 * 每像素：拷入用到的常量 / varying 分量 → 跑 uop（一个 switch，操作数都是 S[字节下标]）→ 输出。
 *
 * 解码结果存在本文件私有的表里，以 code 指针（== &gp_prog_t.code[0]）为键。core.c 每次
 * LOAD_PROGRAM 都先 gp_flush（worker 空闲）再 memcpy + gp_vm_validate，所以同一键的解码结果
 * 只会在没有人执行它时被重写。查不到 / 解码失败（S[] 放不下、内存不足）就走逐条解释的后备路径，
 * 结果与预解码路径逐位相同。
 */

#include "core_internal.h"

/* 设备上强制内联，免得 sin / exp2 留一份不在 IRAM 的副本 */
#if defined(__GNUC__) || defined(__clang__)
#define ESPGPU_VM_FORCE_INLINE 1
#endif
#include "espgpu_vm_math.h"

/* ── 校验 ─────────────────────────────────────────────────────────── */

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

static bool validate_code(const espgpu_vm_insn_t *code, uint32_t n)
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

uint8_t gp_vm_temps_used(const espgpu_vm_insn_t *code, uint32_t n)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        const espgpu_vm_insn_t *in = &code[i];
        if (in->op == ESPGPU_VM_END) {
            break;
        }
        if (in->op == ESPGPU_VM_NOP) {
            continue;
        }
        for (int k = 0; k < 3; k++) {
            if ((in->src[k] & 0xC0) == ESPGPU_VM_BANK_TEMP && (in->src[k] & 0x3Fu) + 1u > m) {
                m = (in->src[k] & 0x3Fu) + 1u;
            }
        }
        if (in->op != ESPGPU_VM_KIL && (in->dst & 0xC0) == ESPGPU_VM_BANK_TEMP &&
            (in->dst & 0x3Fu) + 1u > m) {
            m = (in->dst & 0x3Fu) + 1u;
        }
    }
    return (uint8_t)(m > ESPGPU_VM_TEMPS ? ESPGPU_VM_TEMPS : m);
}

/** 每条指令实际读几个源操作数 */
static const uint8_t k_nsrc[ESPGPU_VM_OP_COUNT] = {
    [ESPGPU_VM_NOP] = 0,  [ESPGPU_VM_MOV] = 1,  [ESPGPU_VM_ADD] = 2,   [ESPGPU_VM_SUB] = 2,
    [ESPGPU_VM_MUL] = 2,  [ESPGPU_VM_MAD] = 3,  [ESPGPU_VM_DIV] = 2,   [ESPGPU_VM_DP2] = 2,
    [ESPGPU_VM_DP3] = 2,  [ESPGPU_VM_DP4] = 2,  [ESPGPU_VM_MIN] = 2,   [ESPGPU_VM_MAX] = 2,
    [ESPGPU_VM_RCP] = 1,  [ESPGPU_VM_RSQ] = 1,  [ESPGPU_VM_SQRT] = 1,  [ESPGPU_VM_EXP2] = 1,
    [ESPGPU_VM_LOG2] = 1, [ESPGPU_VM_POW] = 2,  [ESPGPU_VM_FRC] = 1,   [ESPGPU_VM_FLR] = 1,
    [ESPGPU_VM_ABS] = 1,  [ESPGPU_VM_SLT] = 2,  [ESPGPU_VM_SGE] = 2,   [ESPGPU_VM_SEQ] = 2,
    [ESPGPU_VM_SNE] = 2,  [ESPGPU_VM_LRP] = 3,  [ESPGPU_VM_CMP] = 3,   [ESPGPU_VM_SEL] = 3,
    [ESPGPU_VM_CLAMP] = 3, [ESPGPU_VM_SIN] = 1, [ESPGPU_VM_COS] = 1,   [ESPGPU_VM_TEX] = 1,
    [ESPGPU_VM_KIL] = 1,  [ESPGPU_VM_MOD] = 2,  [ESPGPU_VM_SIGN] = 1,  [ESPGPU_VM_STEP] = 2,
    [ESPGPU_VM_SMOOTH] = 3, [ESPGPU_VM_END] = 0,
};

/* ── 预解码形式 ───────────────────────────────────────────────────── */

/* uop 操作码：标量运算直接沿用 ESPGPU_VM_*（TEX / KIL 也是），另加三个内部码 */
enum {
    U_GEN = ESPGPU_VM_OP_COUNT, /* 带取负的 temp 操作数：x = 原操作码，f = 取负位 */
    U_SAT,                      /* S[d] = sat(S[a]) */
    U_LIMIT,
};

enum { UF_NA = 1, UF_NB = 2, UF_NC = 4 };

typedef struct {
    uint8_t op;
    uint8_t d;      /* 目标 S 下标；TEX 写 d..d+3 */
    uint8_t a, b, c;
    uint8_t x;      /* TEX：纹理单元；GEN：原操作码 */
    uint8_t f;      /* UF_N*：取负（GEN / TEX / KIL） */
    uint8_t pad;
} gp_uop_t;

/** S[] 上限：放不下的程序走后备解释器 */
#define GP_VM_S 128

/* S 的固定格：0 / 1 / -1 / -0（每像素写一次） */
enum { S_ZERO = 0, S_ONE = 1, S_MONE = 2, S_NZERO = 3, S_FIXED = 4 };

typedef struct {
    uint16_t nuop;
    uint8_t nslots;               /* S[] 实际用到的格数 */
    uint8_t nk, nkn, nv, nvn, nz; /* 拷入表长度：常量 / 取负常量 / varying / 取负 varying / 清零 */
    uint8_t kb, knb, vb, vnb;     /* 各拷入表在 S 里的起始下标（连续） */
    uint8_t oslot[4];             /* 输出 o0.xyzw 来自哪个 S 格 */
    uint8_t *ksrc;                /* nk + nkn 个常量分量下标（0..127） */
    uint8_t *vsrc;                /* nv + nvn 个 varying 分量下标（0..31） */
    uint8_t *zs;                  /* nz 个需要清零的 S 下标 */
    gp_uop_t *u;
} gp_vm_dec_t;

/* ── 解码：虚拟寄存器编号（16 位），最后压缩成 S 下标 ──────────────── */

enum {
    V_ZERO = 0, V_ONE = 1, V_MONE = 2, V_NZERO = 3,
    V_SCR = 4,                                   /* 8 个暂存：TEX 结果 4..7，冲突 / 累加 8..11 */
    V_T = 16,                                    /* 16 + t*4 + c */
    V_O = V_T + ESPGPU_VM_TEMPS * 4,             /* o0.xyzw */
    V_K = V_O + 4,                               /* 常量分量 */
    V_KN = V_K + ESPGPU_VM_CONSTS * 4,           /* 取负的常量分量 */
    V_V = V_KN + ESPGPU_VM_CONSTS * 4,           /* varying 分量 */
    V_VN = V_V + ESPGPU_VM_VARYINGS * 4,
    V_COUNT = V_VN + ESPGPU_VM_VARYINGS * 4,
};

typedef struct {
    uint8_t op, x, f;
    uint16_t d, a, b, c;
} vuop_t;

#define GP_VM_MAX_UOPS 1024

typedef struct {
    vuop_t *u;
    uint32_t n;
    bool overflow;
} vbuild_t;

static void vemit(vbuild_t *B, uint8_t op, uint16_t d, uint16_t a, uint16_t b, uint16_t c, uint8_t f,
                  uint8_t x)
{
    if (B->n >= GP_VM_MAX_UOPS) {
        B->overflow = true;
        return;
    }
    vuop_t *u = &B->u[B->n++];
    if (f && op != ESPGPU_VM_TEX && op != ESPGPU_VM_KIL) {
        x = op;
        op = U_GEN;
    }
    u->op = op;
    u->x = x;
    u->f = f;
    u->d = d;
    u->a = a;
    u->b = b;
    u->c = c;
}

/** 源操作数 k 的第 lane 个分量 → 虚拟寄存器；temp 的取负留给 uop（*neg） */
static uint16_t vsrc(const espgpu_vm_insn_t *in, int k, int lane, bool *neg)
{
    const uint8_t r = in->src[k];
    const unsigned comp = (in->swz[k] >> (lane * 2)) & 3u;
    const unsigned idx = r & 0x3Fu;
    const bool ng = (in->neg >> k) & 1u;
    *neg = false;
    switch (r & 0xC0) {
    case ESPGPU_VM_BANK_TEMP:
        *neg = ng;
        return (uint16_t)(V_T + idx * 4 + comp);
    case ESPGPU_VM_BANK_VARYING: return (uint16_t)((ng ? V_VN : V_V) + idx * 4 + comp);
    case ESPGPU_VM_BANK_CONST: return (uint16_t)((ng ? V_KN : V_K) + idx * 4 + comp);
    default:
        if (idx == 1) {
            return ng ? V_MONE : V_ONE;
        }
        return ng ? V_NZERO : V_ZERO;
    }
}

static uint16_t vdst(const espgpu_vm_insn_t *in, int lane)
{
    return in->dst == ESPGPU_VM_OUT ? (uint16_t)(V_O + lane)
                                    : (uint16_t)(V_T + (in->dst & 0x3Fu) * 4 + (unsigned)lane);
}

static unsigned vuop_nsrc(const vuop_t *u)
{
    switch (u->op) {
    case ESPGPU_VM_TEX: return 2;
    case U_SAT: return 1;
    case U_GEN: return k_nsrc[u->x];
    default: return k_nsrc[u->op];
    }
}

static void decode_insn(vbuild_t *B, const espgpu_vm_insn_t *in)
{
    const uint8_t op = in->op;
    const uint8_t mask = in->mask ? (in->mask & 0xF) : 0xF;
    const bool sat = (in->flags & ESPGPU_VM_F_SAT) != 0;
    int lanes[4], nl = 0;
    for (int i = 0; i < 4; i++) {
        if (mask & (1u << i)) {
            lanes[nl++] = i;
        }
    }
    if (op == ESPGPU_VM_KIL) {
        uint16_t seen[4];
        bool seen_neg[4];
        int ns = 0;
        for (int i = 0; i < 4; i++) {
            bool ng;
            const uint16_t s = vsrc(in, 0, i, &ng);
            bool dup = false;
            for (int j = 0; j < ns; j++) {
                dup |= seen[j] == s && seen_neg[j] == ng;
            }
            if (!dup) {
                seen[ns] = s;
                seen_neg[ns++] = ng;
                vemit(B, ESPGPU_VM_KIL, V_ZERO, s, V_ZERO, V_ZERO, ng ? UF_NA : 0, 0);
            }
        }
        return;
    }
    if (nl == 0) {
        return;
    }
    if (op == ESPGPU_VM_TEX) {
        bool nu, nv;
        const uint16_t su = vsrc(in, 0, 0, &nu), sv = vsrc(in, 0, 1, &nv);
        vemit(B, ESPGPU_VM_TEX, V_SCR, su, sv, V_ZERO, (uint8_t)((nu ? UF_NA : 0) | (nv ? UF_NB : 0)),
              in->tex_unit);
        for (int j = 0; j < nl; j++) {
            const uint16_t d = vdst(in, lanes[j]);
            vemit(B, ESPGPU_VM_MOV, d, (uint16_t)(V_SCR + lanes[j]), V_ZERO, V_ZERO, 0, 0);
            if (sat) {
                vemit(B, U_SAT, d, d, V_ZERO, V_ZERO, 0, 0);
            }
        }
        return;
    }
    if (op == ESPGPU_VM_DP2 || op == ESPGPU_VM_DP3 || op == ESPGPU_VM_DP4 || op == ESPGPU_VM_RCP ||
        op == ESPGPU_VM_RSQ) {
        /* 广播：先算一个标量，再复制到其它分量 */
        const int nd = op == ESPGPU_VM_DP2 ? 2 : op == ESPGPU_VM_DP3 ? 3 : op == ESPGPU_VM_DP4 ? 4 : 1;
        uint16_t sa[4], sb[4];
        bool na[4], nb[4];
        for (int i = 0; i < nd; i++) {
            sa[i] = vsrc(in, 0, i, &na[i]);
            sb[i] = vsrc(in, 1, i, &nb[i]);
        }
        uint16_t acc = vdst(in, lanes[0]);
        if (nd > 1) {
            for (int i = 1; i < nd; i++) {
                if (sa[i] == acc || sb[i] == acc) {
                    acc = V_SCR + 4; /* 累加格不能是后面还要读的源 */
                }
            }
            vemit(B, ESPGPU_VM_MUL, acc, sa[0], sb[0], V_ZERO,
                  (uint8_t)((na[0] ? UF_NA : 0) | (nb[0] ? UF_NB : 0)), 0);
            for (int i = 1; i < nd; i++) {
                vemit(B, ESPGPU_VM_MAD, acc, sa[i], sb[i], acc,
                      (uint8_t)((na[i] ? UF_NA : 0) | (nb[i] ? UF_NB : 0)), 0);
            }
        } else {
            vemit(B, op, acc, sa[0], V_ZERO, V_ZERO, na[0] ? UF_NA : 0, 0);
        }
        if (sat) {
            vemit(B, U_SAT, acc, acc, V_ZERO, V_ZERO, 0, 0);
        }
        for (int j = 0; j < nl; j++) {
            const uint16_t d = vdst(in, lanes[j]);
            if (d != acc) {
                vemit(B, ESPGPU_VM_MOV, d, acc, V_ZERO, V_ZERO, 0, 0);
            }
        }
        return;
    }
    /* 逐分量：分量 j 的结果若会被同一条指令后面的分量读到，先写暂存格 */
    const unsigned ns = k_nsrc[op];
    uint16_t s[4][3];
    bool ng[4][3];
    uint16_t dd[4];
    for (int j = 0; j < nl; j++) {
        for (unsigned k = 0; k < 3; k++) {
            s[j][k] = k < ns ? vsrc(in, (int)k, lanes[j], &ng[j][k]) : V_ZERO;
            if (k >= ns) {
                ng[j][k] = false;
            }
        }
        dd[j] = vdst(in, lanes[j]);
    }
    bool spill[4] = {false, false, false, false};
    for (int j = 0; j < nl; j++) {
        for (int l = j + 1; l < nl; l++) {
            for (unsigned k = 0; k < ns; k++) {
                spill[j] |= s[l][k] == dd[j];
            }
        }
    }
    for (int j = 0; j < nl; j++) {
        const uint16_t d = spill[j] ? (uint16_t)(V_SCR + 4 + j) : dd[j];
        vemit(B, op, d, s[j][0], s[j][1], s[j][2],
              (uint8_t)((ng[j][0] ? UF_NA : 0) | (ng[j][1] ? UF_NB : 0) | (ng[j][2] ? UF_NC : 0)), 0);
        if (sat) {
            vemit(B, U_SAT, d, d, V_ZERO, V_ZERO, 0, 0);
        }
    }
    for (int j = 0; j < nl; j++) {
        if (spill[j]) {
            vemit(B, ESPGPU_VM_MOV, dd[j], (uint16_t)(V_SCR + 4 + j), V_ZERO, V_ZERO, 0, 0);
        }
    }
}

static bool vdefs(const vuop_t *u, uint16_t *d0, uint16_t *d1)
{
    if (u->op == ESPGPU_VM_KIL) {
        return false;
    }
    *d0 = u->d;
    *d1 = (uint16_t)(u->op == ESPGPU_VM_TEX ? u->d + 4 : u->d + 1);
    return true;
}

/** 复制传播：纯 MOV d ← s 之后读 d 的地方改读 s（d 或 s 被重写前） */
static void copy_prop(vuop_t *u, uint32_t n, uint16_t oslot[4], uint16_t *map)
{
    uint16_t act[V_COUNT]; /* 当前生效的映射（map[v] != v），重写时只扫这些 */
    unsigned na = 0;
    for (unsigned v = 0; v < V_COUNT; v++) {
        map[v] = (uint16_t)v;
    }
    for (uint32_t i = 0; i < n; i++) {
        vuop_t *x = &u[i];
        const unsigned ns = vuop_nsrc(x);
        if (ns > 0) x->a = map[x->a];
        if (ns > 1) x->b = map[x->b];
        if (ns > 2) x->c = map[x->c];
        uint16_t d0, d1;
        if (!vdefs(x, &d0, &d1)) {
            continue;
        }
        for (unsigned j = 0; j < na;) {
            const uint16_t v = act[j];
            if ((map[v] >= d0 && map[v] < d1) || (v >= d0 && v < d1)) {
                map[v] = v; /* 来源或自身被重写：映射失效 */
                act[j] = act[--na];
            } else {
                j++;
            }
        }
        if (x->op == ESPGPU_VM_MOV && x->a != x->d) {
            map[x->d] = x->a;
            act[na++] = x->d;
        }
    }
    for (int c = 0; c < 4; c++) {
        oslot[c] = map[oslot[c]];
    }
}

static bool commutative(uint8_t op)
{
    return op == ESPGPU_VM_ADD || op == ESPGPU_VM_MUL || op == ESPGPU_VM_MAD || op == ESPGPU_VM_MIN ||
           op == ESPGPU_VM_MAX || op == ESPGPU_VM_SEQ || op == ESPGPU_VM_SNE;
}

static bool same_uop(const vuop_t *x, const vuop_t *y)
{
    if (x->op != y->op || x->x != y->x) {
        return false;
    }
    const unsigned ns = vuop_nsrc(x);
    if (ns > 2 && (x->c != y->c || ((x->f ^ y->f) & UF_NC))) {
        return false;
    }
    const bool fa = (x->f & UF_NA) != 0, fb = (x->f & UF_NB) != 0;
    const bool ga = (y->f & UF_NA) != 0, gb = (y->f & UF_NB) != 0;
    if ((ns < 1 || (x->a == y->a && fa == ga)) && (ns < 2 || (x->b == y->b && fb == gb))) {
        return true;
    }
    /* MAD 只有 a*b 可交换；commutative() 里的其它都是二元 */
    return ns >= 2 && commutative(x->op == U_GEN ? x->x : x->op) && x->a == y->b && x->b == y->a && fa == gb &&
           fb == ga;
}

/** 公共子表达式：与前面某条 uop 完全相同（操作数都没被改写过）就改成 MOV，之后复制传播会消掉它 */
static void cse(vuop_t *u, uint32_t n, uint8_t *def_since)
{
    for (uint32_t i = 0; i < n; i++) {
        vuop_t *x = &u[i];
        if (x->op == ESPGPU_VM_KIL || x->op == ESPGPU_VM_TEX || x->op == ESPGPU_VM_MOV) {
            continue;
        }
        const unsigned ns = vuop_nsrc(x);
        const uint16_t src[3] = {x->a, x->b, x->c};
        memset(def_since, 0, V_COUNT);
        for (uint32_t j = i, steps = 0; j-- > 0 && steps < 128; steps++) {
            const vuop_t *y = &u[j];
            uint16_t d0, d1;
            const bool defs = vdefs(y, &d0, &d1);
            bool clobber = false;
            for (unsigned k = 0; k < ns && defs; k++) {
                clobber |= src[k] >= d0 && src[k] < d1;
            }
            if (clobber) {
                break; /* 更早的同形 uop 读的是旧值 */
            }
            if (defs && d1 == d0 + 1 && !def_since[y->d] && same_uop(x, y)) {
                x->op = ESPGPU_VM_MOV;
                x->f = 0;
                x->a = y->d;
                x->b = x->c = V_ZERO;
                break;
            }
            for (unsigned v = defs ? d0 : 0; defs && v < d1; v++) {
                def_since[v] = 1;
            }
        }
    }
}

/** 死代码删除：结果没人读的 uop 去掉（KIL 除外）。返回剩余条数 */
static uint32_t dce(vuop_t *u, uint32_t n, const uint16_t oslot[4], uint8_t *live)
{
    memset(live, 0, V_COUNT);
    for (int c = 0; c < 4; c++) {
        live[oslot[c]] = 1;
    }
    uint32_t keep = n;
    for (uint32_t i = n; i-- > 0;) {
        vuop_t *x = &u[i];
        uint16_t d0, d1;
        if (vdefs(x, &d0, &d1)) {
            bool any = false;
            for (unsigned v = d0; v < d1; v++) {
                any |= live[v] != 0;
            }
            if (!any || (x->op == ESPGPU_VM_MOV && x->a == x->d)) {
                x->op = ESPGPU_VM_NOP;
                keep--;
                continue;
            }
            for (unsigned v = d0; v < d1; v++) {
                live[v] = 0;
            }
        }
        const unsigned ns = vuop_nsrc(x);
        if (ns > 0) live[x->a] = 1;
        if (ns > 1) live[x->b] = 1;
        if (ns > 2) live[x->c] = 1;
    }
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (u[i].op != ESPGPU_VM_NOP) {
            u[w++] = u[i];
        }
    }
    return keep;
}

static gp_vm_dec_t *decode(const espgpu_vm_insn_t *code, uint32_t n)
{
    gp_vm_dec_t *dp = NULL;
    vbuild_t B = {(vuop_t *)gp_alloc_big(sizeof(vuop_t) * GP_VM_MAX_UOPS), 0, false};
    uint16_t *map = (uint16_t *)gp_alloc_big(sizeof(uint16_t) * V_COUNT);
    uint8_t *aux = (uint8_t *)gp_alloc_big(V_COUNT);  /* live / written / 物理编号 */
    uint8_t *phys = (uint8_t *)gp_alloc_big(V_COUNT);
    if (!B.u || !map || !aux || !phys) {
        goto out;
    }
    uint16_t oslot[4] = {V_ZERO, V_ZERO, V_ZERO, V_ONE};
    for (uint32_t pc = 0; pc < n && !B.overflow; pc++) {
        const espgpu_vm_insn_t *in = &code[pc];
        if (in->op == ESPGPU_VM_END) {
            break;
        }
        if (in->op == ESPGPU_VM_NOP) {
            continue;
        }
        decode_insn(&B, in);
        if (in->op != ESPGPU_VM_KIL && in->dst == ESPGPU_VM_OUT) {
            const uint8_t m = in->mask ? in->mask : 0xF;
            for (int c = 0; c < 4; c++) {
                if (m & (1u << c)) {
                    oslot[c] = (uint16_t)(V_O + c);
                }
            }
        }
    }
    if (B.overflow) {
        goto out;
    }
    copy_prop(B.u, B.n, oslot, map);
    cse(B.u, B.n, aux);
    copy_prop(B.u, B.n, oslot, map);
    B.n = dce(B.u, B.n, oslot, aux);

    /* 先读后写的 temp 分量（每像素清零）；暂存格总是先写后读 */
    uint16_t zl[ESPGPU_VM_TEMPS * 4];
    unsigned nz = 0;
    memset(aux, 0, V_COUNT);
    for (uint32_t i = 0; i < B.n; i++) {
        const vuop_t *x = &B.u[i];
        const unsigned ns = vuop_nsrc(x);
        const uint16_t src[3] = {x->a, x->b, x->c};
        for (unsigned k = 0; k < ns; k++) {
            const uint16_t s = src[k];
            if (s >= V_T && s < V_O && !aux[s]) {
                aux[s] = 1;
                zl[nz++] = s;
            }
        }
        uint16_t d0, d1;
        if (vdefs(x, &d0, &d1)) {
            for (unsigned v = d0; v < d1; v++) {
                aux[v] = 1;
            }
        }
    }
    for (int c = 0; c < 4; c++) {
        if (oslot[c] >= V_T && oslot[c] < V_O && !aux[oslot[c]]) {
            aux[oslot[c]] = 1;
            zl[nz++] = oslot[c]; /* 输出直接取一个从没写过的 temp：0 */
        }
    }

    /* 用到了哪些虚拟寄存器 */
    memset(aux, 0, V_COUNT);
    for (uint32_t i = 0; i < B.n; i++) {
        const vuop_t *x = &B.u[i];
        const unsigned ns = vuop_nsrc(x);
        if (ns > 0) aux[x->a] = 1;
        if (ns > 1) aux[x->b] = 1;
        if (ns > 2) aux[x->c] = 1;
        uint16_t d0, d1;
        if (vdefs(x, &d0, &d1)) {
            for (unsigned v = d0; v < d1; v++) {
                aux[v] = 1;
            }
        }
    }
    for (int c = 0; c < 4; c++) {
        aux[oslot[c]] = 1;
    }
    /* 压缩：固定格 → 拷入表（各自连续）→ 其余 */
    memset(phys, 0xFF, V_COUNT);
    unsigned next = S_FIXED;
    for (unsigned v = 0; v < S_FIXED; v++) {
        phys[v] = (uint8_t)v;
    }
    unsigned cnt[4] = {0, 0, 0, 0};
    unsigned base[4];
    static const uint16_t grp_lo[4] = {V_K, V_KN, V_V, V_VN};
    static const uint16_t grp_hi[4] = {V_KN, V_V, V_VN, V_COUNT};
    for (int g = 0; g < 4; g++) {
        base[g] = next;
        for (unsigned v = grp_lo[g]; v < grp_hi[g]; v++) {
            if (aux[v]) {
                if (next >= GP_VM_S) {
                    goto out;
                }
                phys[v] = (uint8_t)next++;
                cnt[g]++;
            }
        }
    }
    /* 其余：暂存 / temp / o0。TEX 的 4 个结果格（V_SCR..+3）总是一起被标记，按顺序分配自然连续 */
    for (unsigned v = S_FIXED; v < V_K; v++) {
        if (aux[v]) {
            if (next >= GP_VM_S) {
                goto out;
            }
            phys[v] = (uint8_t)next++;
        }
    }

    const size_t nk = cnt[0] + cnt[1], nvv = cnt[2] + cnt[3];
    /* 末尾补一条 END 作哨兵：执行循环不用比较计数 */
    const size_t bytes = sizeof(gp_vm_dec_t) + sizeof(gp_uop_t) * (B.n + 1) + nk + nvv + nz + 8;
    dp = (gp_vm_dec_t *)gp_alloc_fast(bytes);
    if (!dp) {
        goto out;
    }
    memset(dp, 0, bytes);
    dp->u = (gp_uop_t *)(dp + 1);
    dp->ksrc = (uint8_t *)(dp->u + B.n + 1);
    dp->vsrc = dp->ksrc + nk;
    dp->zs = dp->vsrc + nvv;
    dp->nuop = (uint16_t)B.n;
    dp->nslots = (uint8_t)next;
    dp->nk = (uint8_t)cnt[0];
    dp->nkn = (uint8_t)cnt[1];
    dp->nv = (uint8_t)cnt[2];
    dp->nvn = (uint8_t)cnt[3];
    dp->nz = (uint8_t)nz;
    dp->kb = (uint8_t)base[0];
    dp->knb = (uint8_t)base[1];
    dp->vb = (uint8_t)base[2];
    dp->vnb = (uint8_t)base[3];
    {
        size_t ki = 0, vi = 0;
        for (unsigned v = V_K; v < V_V; v++) {
            if (phys[v] != 0xFF) {
                dp->ksrc[ki++] = (uint8_t)((v - (v < V_KN ? V_K : V_KN)));
            }
        }
        for (unsigned v = V_V; v < V_COUNT; v++) {
            if (phys[v] != 0xFF) {
                dp->vsrc[vi++] = (uint8_t)((v - (v < V_VN ? V_V : V_VN)));
            }
        }
    }
    for (unsigned i = 0; i < nz; i++) {
        dp->zs[i] = phys[zl[i]];
    }
    for (int c = 0; c < 4; c++) {
        dp->oslot[c] = phys[oslot[c]];
    }
    for (uint32_t i = 0; i < B.n; i++) {
        const vuop_t *x = &B.u[i];
        gp_uop_t *y = &dp->u[i];
        y->op = x->op;
        y->x = x->x;
        y->f = x->f;
        y->d = x->op == ESPGPU_VM_KIL ? 0 : phys[x->d];
        const unsigned ns = vuop_nsrc(x);
        y->a = ns > 0 ? phys[x->a] : 0;
        y->b = ns > 1 ? phys[x->b] : 0;
        y->c = ns > 2 ? phys[x->c] : 0;
    }
    dp->u[B.n].op = ESPGPU_VM_END;

out:
    gp_free(B.u);
    gp_free(map);
    gp_free(aux);
    gp_free(phys);
    return dp;
}

/* ── 解码缓存：code 指针 → 解码结果 ─────────────────────────────────── */

#define GP_VM_CACHE 32 /* 2 的幂；设备上最多 ESPGPU_VM_MAX_PROGRAMS 个键 */

typedef struct {
    const espgpu_vm_insn_t *key; /* NULL = 空，GP_VM_TOMB = 已删除（探测继续往后走） */
    gp_vm_dec_t *dec;            /* NULL = 解码失败，走后备路径 */
} gp_vm_ent_t;

#define GP_VM_TOMB ((const espgpu_vm_insn_t *)(uintptr_t)1)

/*
 * 开放寻址，只增不搬：插入只填空格 / 墓碑，删除只把键改成墓碑，所以别的核心的 worker 并发查自己的键时
 * 探测链不会断。同一键的解码结果只在它的核心 gp_flush 之后（没有 worker 在跑）才被替换 / 释放。
 */
static gp_vm_ent_t s_cache[GP_VM_CACHE];

static inline unsigned cache_hash(const espgpu_vm_insn_t *key)
{
    return (unsigned)(((uintptr_t)key >> 4) * 2654435761u >> 16) & (GP_VM_CACHE - 1);
}

GP_HOT static inline const gp_vm_dec_t *cache_find(const espgpu_vm_insn_t *key)
{
    unsigned h = cache_hash(key);
    for (unsigned i = 0; i < GP_VM_CACHE; i++, h = (h + 1) & (GP_VM_CACHE - 1)) {
        const gp_vm_ent_t *e = &s_cache[h];
        if (e->key == key) {
            return e->dec;
        }
        if (e->key == NULL) {
            break;
        }
    }
    return NULL;
}

static gp_vm_ent_t *cache_slot(const espgpu_vm_insn_t *key, bool insert)
{
    unsigned h = cache_hash(key);
    gp_vm_ent_t *tomb = NULL;
    for (unsigned i = 0; i < GP_VM_CACHE; i++, h = (h + 1) & (GP_VM_CACHE - 1)) {
        gp_vm_ent_t *e = &s_cache[h];
        if (e->key == key) {
            return e;
        }
        if (e->key == GP_VM_TOMB && tomb == NULL) {
            tomb = e;
        }
        if (e->key == NULL) {
            return insert ? (tomb ? tomb : e) : NULL;
        }
    }
    return insert ? tomb : NULL;
}

static void cache_store(const espgpu_vm_insn_t *key, gp_vm_dec_t *dec)
{
    gp_vm_ent_t *slot = cache_slot(key, true);
    if (slot == NULL) {
        /* 表满（只有模拟器里反复建 / 毁核心且没调 gp_vm_forget 才可能）：不缓存，走后备路径 */
        gp_free(dec);
        return;
    }
    gp_vm_dec_t *old = slot->key == key ? slot->dec : NULL;
    slot->dec = dec;
    slot->key = key;
    gp_free(old);
}

/** 程序内存要释放了（核心销毁）：丢掉它的解码结果。声明待移入 core_internal.h，由 core.c 释放 progs 前调用 */
void gp_vm_forget(const espgpu_vm_insn_t *code);

void gp_vm_forget(const espgpu_vm_insn_t *code)
{
    gp_vm_ent_t *slot = cache_slot(code, false);
    if (slot) {
        gp_free(slot->dec);
        slot->dec = NULL;
        slot->key = GP_VM_TOMB;
    }
}

bool gp_vm_validate(const espgpu_vm_insn_t *code, uint32_t n)
{
    const bool ok = validate_code(code, n);
    /* 失败也要覆盖旧的解码结果（键对应的程序已经换了） */
    cache_store(code, ok ? decode(code, n) : NULL);
    return ok;
}

/* ── 执行 ─────────────────────────────────────────────────────────── */

GP_HOT static inline void tex_fetch(const gp_sampler_t *s, float u, float v, float r[4])
{
    if (s->tex == NULL) {
        r[0] = r[1] = r[2] = r[3] = 1.0f;
        return;
    }
    const uint32_t px = gp_sample(s, u * (float)s->tex->w, v * (float)s->tex->h);
    r[0] = (float)((px >> 16) & 255) * (1.0f / 255.0f);
    r[1] = (float)((px >> 8) & 255) * (1.0f / 255.0f);
    r[2] = (float)(px & 255) * (1.0f / 255.0f);
    r[3] = (float)(px >> 24) * (1.0f / 255.0f);
}

/* 冷路径：不内联、不放 IRAM（op 是运行期值时内联 eval_scalar 会让编译器按 op 把循环复制 30 多份） */
#if defined(__GNUC__) || defined(__clang__)
#define VM_COLD __attribute__((noinline, cold))
#elif defined(_MSC_VER)
#define VM_COLD __declspec(noinline)
#else
#define VM_COLD
#endif

VM_COLD static float eval_slow(uint8_t op, float a, float b, float c)
{
    return espgpu_vm_eval_scalar(op, a, b, c);
}

/** 带取负的 uop（少见） */
VM_COLD static float run_gen(const gp_uop_t *u, const float *S)
{
    const float a = (u->f & UF_NA) ? -S[u->a] : S[u->a];
    const float b = (u->f & UF_NB) ? -S[u->b] : S[u->b];
    const float c = (u->f & UF_NC) ? -S[u->c] : S[u->c];
    return eval_slow(u->x, a, b, c);
}

#if defined(__GNUC__) || defined(__clang__)
#define VM_UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
#define VM_UNREACHABLE() __assume(0)
#else
#define VM_UNREACHABLE() (void)0
#endif

#define U1(OP) \
    case OP: S[u->d] = espgpu_vm_eval_scalar(OP, S[u->a], 0.0f, 0.0f); break
#define U2(OP) \
    case OP: S[u->d] = espgpu_vm_eval_scalar(OP, S[u->a], S[u->b], 0.0f); break
#define U3(OP) \
    case OP: S[u->d] = espgpu_vm_eval_scalar(OP, S[u->a], S[u->b], S[u->c]); break

GP_HOT static bool run_decoded(const gp_vm_dec_t *dp, const float *consts, const gp_sampler_t *smp,
                               const float *v, float out[4])
{
    static const float k_zero[ESPGPU_VM_CONSTS * 4];
    float S[GP_VM_S];
    S[S_ZERO] = 0.0f;
    S[S_ONE] = 1.0f;
    S[S_MONE] = -1.0f;
    S[S_NZERO] = -0.0f;
    const float *k = consts ? consts : k_zero;
    {
        const uint8_t *ks = dp->ksrc;
        float *dst = S + dp->kb;
        for (unsigned i = 0, n = dp->nk; i < n; i++) dst[i] = k[ks[i]];
        ks += dp->nk;
        dst = S + dp->knb;
        for (unsigned i = 0, n = dp->nkn; i < n; i++) dst[i] = -k[ks[i]];
        const uint8_t *vs = dp->vsrc;
        dst = S + dp->vb;
        for (unsigned i = 0, n = dp->nv; i < n; i++) dst[i] = v[vs[i]];
        vs += dp->nv;
        dst = S + dp->vnb;
        for (unsigned i = 0, n = dp->nvn; i < n; i++) dst[i] = -v[vs[i]];
        for (unsigned i = 0, n = dp->nz; i < n; i++) S[dp->zs[i]] = 0.0f;
    }

    /* switch 覆盖 0..U_LIMIT-1 的每个值、default 不可达：编译器省掉范围检查；END 哨兵结束 */
    for (const gp_uop_t *u = dp->u;; u++) {
        switch (u->op) {
        case ESPGPU_VM_NOP:
        case ESPGPU_VM_DP2:
        case ESPGPU_VM_DP3:
        case ESPGPU_VM_DP4: break; /* 解码后不会出现 */
        U1(ESPGPU_VM_MOV);
        U2(ESPGPU_VM_ADD);
        U2(ESPGPU_VM_SUB);
        U2(ESPGPU_VM_MUL);
        U3(ESPGPU_VM_MAD);
        U2(ESPGPU_VM_DIV);
        U2(ESPGPU_VM_MIN);
        U2(ESPGPU_VM_MAX);
        U1(ESPGPU_VM_RCP);
        U1(ESPGPU_VM_RSQ);
        U1(ESPGPU_VM_SQRT);
        U1(ESPGPU_VM_EXP2);
        U1(ESPGPU_VM_LOG2);
        U2(ESPGPU_VM_POW);
        U1(ESPGPU_VM_FRC);
        U1(ESPGPU_VM_FLR);
        U1(ESPGPU_VM_ABS);
        U2(ESPGPU_VM_SLT);
        U2(ESPGPU_VM_SGE);
        U2(ESPGPU_VM_SEQ);
        U2(ESPGPU_VM_SNE);
        U3(ESPGPU_VM_LRP);
        U3(ESPGPU_VM_CMP);
        U3(ESPGPU_VM_SEL);
        U3(ESPGPU_VM_CLAMP);
        U1(ESPGPU_VM_SIN);
        U1(ESPGPU_VM_COS);
        U2(ESPGPU_VM_MOD);
        U1(ESPGPU_VM_SIGN);
        U2(ESPGPU_VM_STEP);
        U3(ESPGPU_VM_SMOOTH);
        case U_SAT: S[u->d] = espgpu_vm_sat(S[u->a]); break;
        case ESPGPU_VM_TEX: {
            const float tu = (u->f & UF_NA) ? -S[u->a] : S[u->a];
            const float tv = (u->f & UF_NB) ? -S[u->b] : S[u->b];
            tex_fetch(&smp[u->x], tu, tv, &S[u->d]);
            break;
        }
        case ESPGPU_VM_KIL: {
            const float a = (u->f & UF_NA) ? -S[u->a] : S[u->a];
            if (a < 0.0f) {
                return false;
            }
            break;
        }
        case U_GEN: S[u->d] = run_gen(u, S); break;
        case ESPGPU_VM_END: goto done;
        default: VM_UNREACHABLE();
        }
    }
done:
    out[0] = S[dp->oslot[0]];
    out[1] = S[dp->oslot[1]];
    out[2] = S[dp->oslot[2]];
    out[3] = S[dp->oslot[3]];
    return true;
}

/* ── 后备：逐条解释原始指令（与预解码路径逐位相同；放不进 S[] 的大程序用） ── */

static const float k_zero4[4] = {0, 0, 0, 0};
static const float k_one4[4] = {1, 1, 1, 1};

static inline const float *reg_ptr(uint8_t r, const float *regs, const float *v, const float *k)
{
    const uint8_t idx = r & 0x3F;
    switch (r & 0xC0) {
    case ESPGPU_VM_BANK_TEMP: return regs + idx * 4;
    case ESPGPU_VM_BANK_VARYING: return v + idx * 4;
    case ESPGPU_VM_BANK_CONST: return k ? k + idx * 4 : k_zero4;
    default: return idx ? k_one4 : k_zero4;
    }
}

static inline void load_src(const espgpu_vm_insn_t *in, int i, const float *regs, const float *v,
                            const float *k, float o[4])
{
    const float *p = reg_ptr(in->src[i], regs, v, k);
    const uint8_t sw = in->swz[i];
    const bool ng = (in->neg >> i) & 1u;
    for (int c = 0; c < 4; c++) {
        const float x = p[(sw >> (c * 2)) & 3];
        o[c] = ng ? -x : x;
    }
}

VM_COLD static bool run_wire(const gp_prog_t *p, const float *consts, const gp_sampler_t *smp, const float *v,
                             float *regs, float out[4])
{
    float o[4] = {0, 0, 0, 1};
    memset(regs, 0, (size_t)p->ntemps * 4 * sizeof(float));
    for (uint32_t pc = 0; pc < p->count; pc++) {
        const espgpu_vm_insn_t *in = &p->code[pc];
        const uint8_t op = in->op;
        if (op == ESPGPU_VM_END) {
            break;
        }
        if (op == ESPGPU_VM_NOP) {
            continue;
        }
        float a[4], b[4] = {0, 0, 0, 0}, c[4] = {0, 0, 0, 0}, r[4] = {0, 0, 0, 0};
        const uint8_t ns = k_nsrc[op];
        const uint8_t m = in->mask ? in->mask : 0xF;
        load_src(in, 0, regs, v, consts, a);
        if (ns >= 2) load_src(in, 1, regs, v, consts, b);
        if (ns >= 3) load_src(in, 2, regs, v, consts, c);
        switch (op) {
        case ESPGPU_VM_KIL:
            if (a[0] < 0.0f || a[1] < 0.0f || a[2] < 0.0f || a[3] < 0.0f) {
                return false;
            }
            continue;
        case ESPGPU_VM_TEX: tex_fetch(&smp[in->tex_unit], a[0], a[1], r); break;
        case ESPGPU_VM_DP2:
        case ESPGPU_VM_DP3:
        case ESPGPU_VM_DP4:
            r[0] = r[1] = r[2] = r[3] = espgpu_vm_dot(op == ESPGPU_VM_DP2 ? 2 : op == ESPGPU_VM_DP3 ? 3 : 4, a, b);
            break;
        case ESPGPU_VM_RCP:
        case ESPGPU_VM_RSQ: r[0] = r[1] = r[2] = r[3] = eval_slow(op, a[0], 0.0f, 0.0f); break;
        default:
            for (int i = 0; i < 4; i++) {
                if (m & (1u << i)) {
                    r[i] = eval_slow(op, a[i], b[i], c[i]);
                }
            }
            break;
        }
        if (in->flags & ESPGPU_VM_F_SAT) {
            for (int i = 0; i < 4; i++) r[i] = espgpu_vm_sat(r[i]);
        }
        float *d = in->dst == ESPGPU_VM_OUT ? o : regs + (in->dst & 0x3F) * 4;
        for (int i = 0; i < 4; i++) {
            if (m & (1u << i)) d[i] = r[i];
        }
    }
    out[0] = o[0];
    out[1] = o[1];
    out[2] = o[2];
    out[3] = o[3];
    return true;
}

GP_HOT bool gp_vm_run(const gp_prog_t *p, const float *consts, const gp_sampler_t *smp,
                      const float *v, float *regs, float out[4])
{
    const gp_vm_dec_t *dp = cache_find(p->code);
    if (dp) {
        return run_decoded(dp, consts, smp, v, out);
    }
    return run_wire(p, consts, smp, v, regs, out);
}

/* ── 一段片元（≤ GP_VM_SPAN 个）一起执行 ─────────────────────────────
 *
 * 逐条 uop 对 n 个像素循环：取 uop、分派、解析操作数的开销摊到 n 个像素上。
 * 常量 / 0 / 1 这类一致操作数直接读（步长 0），varying / temp / o0 在 S 里按 [格][像素] 存。
 * 段内 varying 是线性的（raster3d 的段内插值就是 va += vstep），所以只传起点 v0 和步长 dv：
 * 第 i 个像素的 v = v0 累加 i 次 dv（与逐像素累加逐位相同）。
 */

#define GP_VM_SPAN 8          /* = raster3d.c 的 SEG */
#define GP_VM_SPAN_SLOTS 24   /* 非一致格上限：S 占 24×8×4 = 768 B 栈，超出走逐像素 */
#define GP_VM_SPAN_KN 16      /* 取负常量分量上限 */

/* 不加 const：放 .data（设备上是内部 DRAM），热循环里每个 0 / 1 操作数都读它，不走 flash 缓存 */
static float k_special[S_FIXED] = {0.0f, 1.0f, -1.0f, -0.0f};

GP_HOT static inline const float *span_src(uint8_t s, const gp_vm_dec_t *dp, const float *k, const float *kn,
                                           float *S, unsigned *st)
{
    if (s >= dp->vb) {
        *st = 1;
        return S + (size_t)(s - dp->vb) * GP_VM_SPAN;
    }
    *st = 0;
    if (s < S_FIXED) {
        return k_special + s;
    }
    if (s < dp->knb) {
        return k + dp->ksrc[s - dp->kb];
    }
    return kn + (s - dp->knb);
}

/* 一元 / 二元 / 三元：只推进用到的操作数指针。不展开：Xtensa 有零开销循环，展开只会撑大 IRAM */
#if defined(__GNUC__) || defined(__clang__)
#define SP_NOUNROLL _Pragma("GCC unroll 1")
#else
#define SP_NOUNROLL
#endif
#define SP_LOOP1(EXPR)                         \
    {                                          \
        SP_NOUNROLL                            \
        for (int i = 0; i < n; i++, A += sa) { \
            D[i] = (EXPR);                     \
        }                                      \
    }                                          \
    break
#define SP_LOOP2(EXPR)                                   \
    {                                                    \
        SP_NOUNROLL                                      \
        for (int i = 0; i < n; i++, A += sa, Bp += sb) { \
            D[i] = (EXPR);                               \
        }                                                \
    }                                                    \
    break
#define SP_LOOP3(EXPR)                                            \
    {                                                             \
        SP_NOUNROLL                                               \
        for (int i = 0; i < n; i++, A += sa, Bp += sb, C += sc) { \
            D[i] = (EXPR);                                        \
        }                                                         \
    }                                                             \
    break
#define SP1(OP) case OP: SP_LOOP1(espgpu_vm_eval_scalar(OP, *A, 0.0f, 0.0f))
#define SP2(OP) case OP: SP_LOOP2(espgpu_vm_eval_scalar(OP, *A, *Bp, 0.0f))
#define SP3(OP) case OP: SP_LOOP3(espgpu_vm_eval_scalar(OP, *A, *Bp, *C))

GP_HOT static uint32_t run_span(const gp_vm_dec_t *dp, const float *consts, const gp_sampler_t *smp,
                                const float *v0, const float *dv, int n, float (*out)[4])
{
    static const float k_zero[ESPGPU_VM_CONSTS * 4];
    float S[GP_VM_SPAN_SLOTS * GP_VM_SPAN];
    float kn[GP_VM_SPAN_KN];
    const float *k = consts ? consts : k_zero;
    for (unsigned i = 0, m = dp->nkn; i < m; i++) {
        kn[i] = -k[dp->ksrc[dp->nk + i]];
    }
    /* varying 列：起点累加步长 */
    {
        const uint8_t *vs = dp->vsrc;
        float *col = S;
        for (unsigned j = 0, m = dp->nv + dp->nvn; j < m; j++, col += GP_VM_SPAN) {
            const bool ng = j >= dp->nv;
            float x = v0[vs[j]];
            const float d = dv[vs[j]];
            for (int i = 0; i < n; i++) {
                col[i] = ng ? -x : x;
                x += d;
            }
        }
        for (unsigned z = 0, m = dp->nz; z < m; z++) {
            float *zc = S + (size_t)(dp->zs[z] - dp->vb) * GP_VM_SPAN;
            for (int i = 0; i < n; i++) zc[i] = 0.0f;
        }
    }

    uint32_t alive = (1u << n) - 1u;
    for (const gp_uop_t *u = dp->u;; u++) {
        unsigned sa, sb, sc;
        const float *A = span_src(u->a, dp, k, kn, S, &sa);
        const float *Bp = span_src(u->b, dp, k, kn, S, &sb);
        const float *C = span_src(u->c, dp, k, kn, S, &sc);
        /* 目标总是 temp / 暂存 / o0（d ≥ vb）；KIL / END 的 d 是 0，不用 */
        float *D = u->d >= dp->vb ? S + (size_t)(u->d - dp->vb) * GP_VM_SPAN : S;
        switch (u->op) {
        case ESPGPU_VM_NOP:
        case ESPGPU_VM_DP2:
        case ESPGPU_VM_DP3:
        case ESPGPU_VM_DP4: break;
        SP1(ESPGPU_VM_MOV);
        SP2(ESPGPU_VM_ADD);
        SP2(ESPGPU_VM_SUB);
        SP2(ESPGPU_VM_MUL);
        SP3(ESPGPU_VM_MAD);
        SP2(ESPGPU_VM_DIV);
        SP2(ESPGPU_VM_MIN);
        SP2(ESPGPU_VM_MAX);
        SP1(ESPGPU_VM_RCP);
        SP1(ESPGPU_VM_RSQ);
        SP1(ESPGPU_VM_SQRT);
        SP1(ESPGPU_VM_EXP2);
        SP1(ESPGPU_VM_LOG2);
        SP2(ESPGPU_VM_POW);
        SP1(ESPGPU_VM_FRC);
        SP1(ESPGPU_VM_FLR);
        SP1(ESPGPU_VM_ABS);
        SP2(ESPGPU_VM_SLT);
        SP2(ESPGPU_VM_SGE);
        SP2(ESPGPU_VM_SEQ);
        SP2(ESPGPU_VM_SNE);
        SP3(ESPGPU_VM_LRP);
        SP3(ESPGPU_VM_CMP);
        SP3(ESPGPU_VM_SEL);
        SP3(ESPGPU_VM_CLAMP);
        SP1(ESPGPU_VM_SIN);
        SP1(ESPGPU_VM_COS);
        SP2(ESPGPU_VM_MOD);
        SP1(ESPGPU_VM_SIGN);
        SP2(ESPGPU_VM_STEP);
        SP3(ESPGPU_VM_SMOOTH);
        case U_SAT: SP_LOOP1(espgpu_vm_sat(*A));
        case U_GEN: {
            for (int i = 0; i < n; i++) {
                const float a = (u->f & UF_NA) ? -*A : *A;
                const float b = (u->f & UF_NB) ? -*Bp : *Bp;
                const float c = (u->f & UF_NC) ? -*C : *C;
                D[i] = eval_slow(u->x, a, b, c);
                A += sa;
                Bp += sb;
                C += sc;
            }
            break;
        }
        case ESPGPU_VM_TEX:
            for (int i = 0; i < n; i++) {
                float r[4];
                const float tu = (u->f & UF_NA) ? -*A : *A;
                const float tv = (u->f & UF_NB) ? -*Bp : *Bp;
                tex_fetch(&smp[u->x], tu, tv, r);
                D[i] = r[0];
                D[i + GP_VM_SPAN] = r[1];
                D[i + 2 * GP_VM_SPAN] = r[2];
                D[i + 3 * GP_VM_SPAN] = r[3];
                A += sa;
                Bp += sb;
            }
            break;
        case ESPGPU_VM_KIL:
            for (int i = 0; i < n; i++, A += sa) {
                const float a = (u->f & UF_NA) ? -*A : *A;
                if (a < 0.0f) {
                    alive &= ~(1u << i);
                }
            }
            if (alive == 0) {
                return 0;
            }
            break;
        case ESPGPU_VM_END: goto done;
        default: VM_UNREACHABLE();
        }
    }
done:
    for (int c = 0; c < 4; c++) {
        unsigned st;
        const float *P = span_src(dp->oslot[c], dp, k, kn, S, &st);
        for (int i = 0; i < n; i++, P += st) {
            out[i][c] = *P;
        }
    }
    return alive;
}

/** 放不进整段执行的程序：逐像素（冷路径，不放 IRAM） */
VM_COLD static uint32_t span_fallback(const gp_prog_t *p, const float *consts, const gp_sampler_t *smp,
                                      const float *v0, const float *dv, int n, float *regs, float (*out)[4])
{
    float v[ESPGPU_VM_VARYINGS * 4];
    memcpy(v, v0, sizeof(v));
    uint32_t kept = 0;
    for (int i = 0; i < n; i++) {
        if (gp_vm_run(p, consts, smp, v, regs, out[i])) {
            kept |= 1u << i;
        }
        for (unsigned j = 0; j < ESPGPU_VM_VARYINGS * 4; j++) {
            v[j] += dv[j];
        }
    }
    return kept;
}

/**
 * 一段 n（1..GP_VM_SPAN）个片元：第 i 个的 varying = v0 累加 i 次 dv（各 ESPGPU_VM_VARYINGS*4 个 float，
 * 布局同 gp_vm_run 的 v）。out[i] 为第 i 个的颜色；返回未被 KIL 的片元位掩码（bit i）。
 * 结果与逐个调 gp_vm_run 逐位相同。（声明待移入 core_internal.h，见 raster3d 接入说明）
 */
uint32_t gp_vm_run_span(const gp_prog_t *p, const float *consts, const gp_sampler_t *smp, const float *v0,
                        const float *dv, int n, float *regs, float (*out)[4]);

GP_HOT uint32_t gp_vm_run_span(const gp_prog_t *p, const float *consts, const gp_sampler_t *smp,
                               const float *v0, const float *dv, int n, float *regs, float (*out)[4])
{
    if (n <= 0) {
        return 0;
    }
    if (n > GP_VM_SPAN) {
        n = GP_VM_SPAN;
    }
    const gp_vm_dec_t *dp = cache_find(p->code);
    if (dp && dp->nslots - dp->vb <= GP_VM_SPAN_SLOTS && dp->nkn <= GP_VM_SPAN_KN) {
        return run_span(dp, consts, smp, v0, dv, n, out);
    }
    return span_fallback(p, consts, smp, v0, dv, n, regs, out);
}
