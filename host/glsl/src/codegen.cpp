/*
 * codegen.cpp — 片元着色器：模式识别 → 内置着色模式；否则生成 VM 字节码
 *
 * 流程：
 *   1. 可达性 + "只依赖 uniform / 常量"（uo）分类
 *   2. 模式识别：输出颜色拆成乘法因子（纹理 / 顶点色 / uo 因子），能映射到
 *      ESPGPU_SHADE_* 就不走 VM。uo 因子在主机上算好：没有顶点色时当常量色下发，
 *      有顶点色时由运行时逐顶点乘进顶点色（插值是线性的，结果相同）。
 *   3. 窥孔改写：SUB(0, x) 折进源取负修饰、MUL + ADD/SUB → MAD、
 *      SEL(x < 0, a, b) → CMP、KIL(x < 0) 直接用 x
 *   4. uo 子表达式在主机上每次绘制算一次（preshader，见 fragment_consts），结果放进 c 槽
 *   5. 发射：按拓扑序线性扫描。寄存器按分量分配——同一个操作数要用到的几个结果尽量
 *      放进同一个寄存器的不同分量（省掉拼装 MOV）；只给输出颜色用的结果直接写进 o0；
 *      o0 初值 (0,0,0,1)，颜色里等于初值的常量分量不再写。
 *
 * 生成的代码从不读未写过的临时寄存器分量（设备在像素间不清零 temps）。
 * 寄存器或常量槽不够时依次关掉 3/4 的优化重试。
 */

#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace espgpu::glsl {

namespace {

bool componentwise(uint8_t op)
{
    switch (op) {
    case ESPGPU_VM_DP2:
    case ESPGPU_VM_DP3:
    case ESPGPU_VM_DP4:
    case ESPGPU_VM_RCP:
    case ESPGPU_VM_RSQ: return false;
    default: return true;
    }
}

int popcount4(unsigned m)
{
    return static_cast<int>((m & 1u) + ((m >> 1) & 1u) + ((m >> 2) & 1u) + ((m >> 3) & 1u));
}

bool same_lane(const PreLane &a, const PreLane &b)
{
    if (a.node >= 0 || b.node >= 0) return a.node == b.node && a.comp == b.comp;
    return std::memcmp(&a.k, &b.k, 4) == 0;
}

/**
 * alpha 测试参考值：片元 alpha 是字节 b/255，GLSL 写的是 `if (a OP k) discard;`。
 * 求出与之逐字节等价的设备比较（keep = 8 位 alpha FUNC ref）。
 */
void alpha_ref_for(uint8_t vm_op, bool alpha_first, float k, uint8_t &func, uint8_t &ref)
{
    auto discard = [&](int b) {
        const float a = static_cast<float>(b) * (1.0f / 255.0f);
        const float x = alpha_first ? a : k, y = alpha_first ? k : a;
        return vm_op == ESPGPU_VM_SLT ? x < y : x >= y;
    };
    /* 丢弃集合是 [0, t) 或 [t, 255] 的形式（比较对 a 单调） */
    int first_keep = -1, last_keep = -1;
    for (int b = 0; b < 256; b++) {
        if (!discard(b)) {
            if (first_keep < 0) first_keep = b;
            last_keep = b;
        }
    }
    if (first_keep < 0) {
        func = ESPGPU_CMP_NEVER;
        ref = 0;
    } else if (first_keep == 0 && last_keep == 255) {
        func = ESPGPU_CMP_ALWAYS;
        ref = 0;
    } else if (last_keep == 255) {
        func = ESPGPU_CMP_GEQUAL; /* 保留 [first_keep, 255] */
        ref = static_cast<uint8_t>(first_keep);
    } else {
        func = ESPGPU_CMP_LEQUAL; /* 保留 [0, last_keep] */
        ref = static_cast<uint8_t>(last_keep);
    }
}

struct Gen {
    Shader &sh;
    Dag &d;
    std::string err;
    bool use_pre = true;  /**< uo 子表达式放到主机预计算 */
    bool use_pack = true; /**< 按操作数把结果打包进同一寄存器 */
    bool use_slp = true;  /**< 同构标量运算合并成向量运算 */

    std::vector<int> live;
    std::vector<int> uses;        /* 每个节点被引用的次数（每个操作数里去重） */
    std::vector<uint8_t> lanes;   /* 每个节点被引用到的逻辑分量 */
    std::vector<char> uo;         /* 只依赖 uniform / 常量 */

    /* 常量表扩展槽：c[uniform_slot_count + i] */
    struct XSlot {
        PreLane l[4];
        int n = 0;
    };
    std::vector<XSlot> xs;

    /* 寄存器分配 */
    int owner[ESPGPU_VM_TEMPS][4];
    int resv[ESPGPU_VM_TEMPS][4];
    struct Place {
        uint8_t reg = 0;
        int8_t lane[4] = {-1, -1, -1, -1}; /* 逻辑分量 → 物理分量 */
    };
    std::vector<Place> place;
    struct Group {
        int lanes = 0;
        int reg = -1;
    };
    std::vector<Group> groups;
    std::vector<int> group_of;
    std::vector<uint8_t> out_mask;              /* 直接写 o0 的节点：写哪些分量 */
    std::vector<std::array<int8_t, 4>> out_map; /* o0 分量 → 逻辑分量 */
    std::vector<int> vreg_of_input;
    std::vector<int> unit_of_binding;

    Gen(Shader &s) : sh(s), d(*s.dag)
    {
        for (auto &r : owner)
            for (int &x : r) x = -1;
        for (auto &r : resv)
            for (int &x : r) x = -1;
    }

    bool fail(const std::string &m)
    {
        if (err.empty()) err = m;
        return false;
    }

    const Node &N(int id) const { return d.nodes[static_cast<size_t>(id)]; }
    Node &NM(int id) { return d.nodes[static_cast<size_t>(id)]; }

    bool isK(const Scalar &s, float *v = nullptr) const
    {
        const Node &n = N(s.node);
        if (n.kind != NK::Const) return false;
        if (v) *v = n.c;
        return true;
    }
    bool isK(const Scalar &s, float want) const
    {
        float v;
        return isK(s, &v) && v == want;
    }

    bool pre(int id) const { return use_pre && uo[static_cast<size_t>(id)] && N(id).kind == NK::Op; }
    bool emits(int id) const
    {
        const Node &n = N(id);
        return (n.kind == NK::Op || n.kind == NK::Tex || n.kind == NK::Kill) && !pre(id);
    }

    /* ── 可达性 / 分类 ── */

    void classify()
    {
        uo.assign(d.nodes.size(), 0);
        for (size_t id = 0; id < d.nodes.size(); id++) {
            const Node &n = d.nodes[id];
            bool u = n.kind == NK::Const || n.kind == NK::Uniform;
            if (n.kind == NK::Op) {
                u = true;
                for (int k = 0; k < n.nargs; k++)
                    for (int i = 0; i < n.a[k].n; i++) u = u && uo[static_cast<size_t>(n.a[k].s[i].node)];
            }
            uo[id] = u;
        }
    }

    void count(const Operand &o)
    {
        int seen[4];
        int ns = 0;
        for (int i = 0; i < o.n; i++) {
            const int id = o.s[i].node;
            lanes[static_cast<size_t>(id)] |= static_cast<uint8_t>(1u << o.s[i].comp);
            bool dup = false;
            for (int k = 0; k < ns; k++) dup = dup || seen[k] == id;
            if (!dup) {
                seen[ns++] = id;
                uses[static_cast<size_t>(id)]++;
            }
        }
    }

    void reach()
    {
        const size_t n = d.nodes.size();
        std::vector<char> seen(n, 0);
        std::vector<int> stack;
        auto push_op = [&](const Operand &o) {
            for (int i = 0; i < o.n; i++) stack.push_back(o.s[i].node);
        };
        if (d.has_color) push_op(d.color);
        for (int k : d.kills) stack.push_back(k);
        while (!stack.empty()) {
            const int id = stack.back();
            stack.pop_back();
            if (id < 0 || seen[static_cast<size_t>(id)]) continue;
            seen[static_cast<size_t>(id)] = 1;
            const Node &nd = N(id);
            for (int k = 0; k < nd.nargs; k++) push_op(nd.a[k]);
        }
        live.clear();
        uses.assign(n, 0);
        lanes.assign(n, 0);
        for (auto &in : sh.inputs) in.used = false;
        for (auto &u : sh.uniforms) u.used = false;
        for (size_t id = 0; id < n; id++) {
            if (!seen[id]) continue;
            const Node &nd = d.nodes[id];
            live.push_back(static_cast<int>(id));
            for (int k = 0; k < nd.nargs; k++) count(nd.a[k]);
            if (nd.kind == NK::Varying && nd.index >= 0) sh.inputs[static_cast<size_t>(nd.index)].used = true;
            if (nd.kind == NK::Uniform) mark_uniform_slot(nd.index);
            if (nd.kind == NK::Tex && nd.index >= 0) {
                sh.uniforms[static_cast<size_t>(d.samplers[static_cast<size_t>(nd.index)].first)].used = true;
            }
        }
        if (d.has_color) count(d.color);
    }

    void mark_uniform_slot(int slot)
    {
        for (auto &u : sh.uniforms) {
            if (!u.is_sampler && slot >= u.local_slot && slot < u.local_slot + u.array_size * u.columns) u.used = true;
        }
    }

    int single_node(const Operand &o) const
    {
        if (o.n == 0) return -1;
        for (int i = 1; i < o.n; i++) {
            if (o.s[i].node != o.s[0].node) return -1;
        }
        return o.s[0].node;
    }

    /** o 的各分量取的是某节点的哪些分量，把它们换成该节点操作数 src 对应分量 */
    static Operand compose(const Operand &src, const Operand &o)
    {
        Operand r;
        r.n = o.n;
        r.neg = src.neg;
        for (int i = 0; i < o.n; i++) r.s[i] = src.s[std::min<int>(o.s[i].comp, src.n - 1)];
        return r;
    }

    /* ── 常量表扩展槽 ── */

    /** 把一组分量放进同一个扩展槽（尽量复用已有的槽），返回槽号与每个分量的位置 */
    int xplace(const std::vector<PreLane> &want, int lane_of[4])
    {
        std::vector<PreLane> uniq;
        for (const auto &w : want) {
            bool dup = false;
            for (const auto &u : uniq) dup = dup || same_lane(u, w);
            if (!dup) uniq.push_back(w);
        }
        auto find = [](const XSlot &x, const PreLane &w) {
            for (int i = 0; i < x.n; i++) {
                if (same_lane(x.l[i], w)) return i;
            }
            return -1;
        };
        int slot = -1, best_missing = 5;
        for (size_t s = 0; s < xs.size(); s++) {
            int missing = 0;
            for (const auto &w : uniq) missing += find(xs[s], w) < 0;
            if (xs[s].n + missing <= 4 && missing < best_missing) {
                best_missing = missing;
                slot = static_cast<int>(s);
            }
        }
        if (slot < 0) {
            xs.emplace_back();
            slot = static_cast<int>(xs.size()) - 1;
        }
        XSlot &x = xs[static_cast<size_t>(slot)];
        for (const auto &w : uniq) {
            if (find(x, w) < 0) x.l[x.n++] = w;
        }
        for (size_t i = 0; i < want.size(); i++) lane_of[i] = find(x, want[i]);
        return slot;
    }

    PreLane lane_of_scalar(const Scalar &s) const
    {
        PreLane l;
        if (isK(s, &l.k)) {
            if (l.k == 0.0f) l.k = 0.0f;
            return l;
        }
        l.node = s.node;
        l.comp = s.comp;
        return l;
    }

    uint8_t xreg(int slot) const
    {
        return ESPGPU_VM_REG(ESPGPU_VM_BANK_CONST, sh.uniform_slot_count + slot);
    }

    /** 整个 uo 操作数对应的 c 槽：恰好是一个 vec4 uniform 时直接用它，否则放进扩展槽 */
    int konst_slot(const Operand &o)
    {
        const int u = single_node(o);
        if (u >= 0 && N(u).kind == NK::Uniform && o.n == 4 && !o.neg) {
            bool ident = true;
            for (int i = 0; i < 4; i++) ident = ident && o.s[i].comp == i;
            if (ident) return N(u).index;
        }
        /* 带取负的操作数不会出现在模式里（模式识别在改写之前） */
        std::vector<PreLane> want;
        for (int i = 0; i < o.n; i++) want.push_back(lane_of_scalar(o.s[i]));
        int lo[4];
        const int slot = xplace(want, lo);
        /* 模式要求 xyzw 恒等：放进一个新槽保证顺序 */
        bool ident = true;
        for (int i = 0; i < o.n; i++) ident = ident && lo[i] == i;
        if (ident && o.n == 4) return sh.uniform_slot_count + slot;
        xs.emplace_back();
        XSlot &x = xs.back();
        for (int i = 0; i < 4; i++) x.l[i] = want[static_cast<size_t>(std::min<int>(i, o.n - 1))];
        x.n = 4;
        return sh.uniform_slot_count + static_cast<int>(xs.size()) - 1;
    }

    /** 收尾：扩展槽 → Dag::xslots，主机预计算需要的节点 → Dag::pre_order */
    bool finalize_consts()
    {
        FragmentPlan &fp = sh.frag;
        fp.uniform_slots = sh.uniform_slot_count;
        fp.const_slots = sh.uniform_slot_count + static_cast<int>(xs.size());
        if (fp.const_slots > static_cast<int>(ESPGPU_VM_CONSTS)) {
            return fail("uniforms + constants exceed " + std::to_string(ESPGPU_VM_CONSTS) + " vec4 registers (" +
                        std::to_string(fp.const_slots) + ")");
        }
        d.xslots.clear();
        std::vector<char> need(d.nodes.size(), 0);
        std::vector<int> stack;
        for (const auto &x : xs) {
            std::array<PreLane, 4> a{};
            for (int i = 0; i < 4; i++) {
                a[static_cast<size_t>(i)] = i < x.n ? x.l[i] : PreLane{};
                if (i < x.n && x.l[i].node >= 0) stack.push_back(x.l[i].node);
            }
            d.xslots.push_back(a);
        }
        while (!stack.empty()) {
            const int id = stack.back();
            stack.pop_back();
            if (need[static_cast<size_t>(id)]) continue;
            need[static_cast<size_t>(id)] = 1;
            const Node &n = N(id);
            for (int k = 0; k < n.nargs; k++)
                for (int i = 0; i < n.a[k].n; i++) stack.push_back(n.a[k].s[i].node);
        }
        d.pre_order.clear();
        fp.preshader_nodes = 0;
        for (size_t id = 0; id < d.nodes.size(); id++) {
            if (!need[id]) continue;
            d.pre_order.push_back(static_cast<int>(id));
            fp.preshader_nodes += d.nodes[id].kind == NK::Op;
        }
        return true;
    }

    /* ── 模式识别 ── */

    bool all_uo(const Operand &o) const
    {
        for (int i = 0; i < o.n; i++) {
            if (!uo[static_cast<size_t>(o.s[i].node)]) return false;
        }
        return true;
    }

    /** 一个分量拆成的乘法因子 */
    struct LaneFactor {
        enum { Tex, Color, Konst } kind;
        int binding = -1; /* Tex：采样器绑定 */
        int input = -1;   /* Tex：坐标 varying；Color：颜色 varying */
        int comp = 0;     /* Tex / Color：取的分量 */
        Scalar k;         /* Konst */
    };

    bool lane_factors(const Scalar &s, std::vector<LaneFactor> &out, int depth = 0) const
    {
        if (depth > 16) return false;
        if (uo[static_cast<size_t>(s.node)]) {
            LaneFactor f{LaneFactor::Konst, -1, -1, 0, {}};
            f.k = s;
            out.push_back(f);
            return true;
        }
        const Node &n = N(s.node);
        if (n.kind == NK::Varying && n.index >= 0) {
            LaneFactor f{LaneFactor::Color, -1, -1, 0, {}};
            f.input = n.index;
            f.comp = s.comp;
            out.push_back(f);
            return true;
        }
        if (n.kind == NK::Tex) {
            const Operand &c = n.a[0];
            if (c.n != 2 || c.neg || c.s[0].node != c.s[1].node || c.s[0].comp != 0 || c.s[1].comp != 1) return false;
            const Node &v = N(c.s[0].node);
            if (v.kind != NK::Varying || v.index < 0) return false;
            LaneFactor f{LaneFactor::Tex, -1, -1, 0, {}};
            f.binding = n.index;
            f.input = v.index;
            f.comp = s.comp;
            out.push_back(f);
            return true;
        }
        if (n.kind == NK::Op && n.op == ESPGPU_VM_MUL && !n.a[0].neg && !n.a[1].neg) {
            return lane_factors(n.a[0].s[s.comp], out, depth + 1) && lane_factors(n.a[1].s[s.comp], out, depth + 1);
        }
        return false;
    }

    struct Factors {
        std::vector<std::pair<int, int>> tex; /* (绑定, 坐标 varying) */
        int color = -1;                       /* 颜色 varying */
        std::vector<Operand> konst;           /* 每个是 4 分量的 uo 操作数，逐分量相乘 */
    };

    /** 输出颜色逐分量拆因子，四个分量的纹理 / 顶点色因子必须一致（设备按分量相乘） */
    bool factors(const Operand &o, Factors &fs) const
    {
        if (o.n != 4 || o.neg) return false;
        std::vector<LaneFactor> lf[4];
        for (int i = 0; i < 4; i++) {
            if (!lane_factors(o.s[i], lf[i])) return false;
        }
        /* 纹理：每个分量都要有同一组纹理，且取的就是本分量 */
        auto texes = [&](int i) {
            std::vector<std::pair<int, int>> t;
            for (const auto &f : lf[i]) {
                if (f.kind == LaneFactor::Tex) t.push_back({f.binding, f.input});
            }
            std::sort(t.begin(), t.end());
            return t;
        };
        fs.tex = texes(0);
        for (int i = 0; i < 4; i++) {
            if (texes(i) != fs.tex) return false;
            for (const auto &f : lf[i]) {
                if (f.kind == LaneFactor::Tex && f.comp != i) return false;
            }
        }
        /* 顶点色：同一个 varying、取本分量；3 分量 varying 的 alpha 没有因子（设备补 1） */
        for (int i = 0; i < 4; i++) {
            int cnt = 0;
            for (const auto &f : lf[i]) {
                if (f.kind != LaneFactor::Color) continue;
                if (++cnt > 1 || f.comp != i || (fs.color >= 0 && fs.color != f.input)) return false;
                fs.color = f.input;
            }
        }
        if (fs.color >= 0) {
            const int w = sh.inputs[static_cast<size_t>(fs.color)].components;
            for (int i = 0; i < 4; i++) {
                bool has = false;
                for (const auto &f : lf[i]) has = has || f.kind == LaneFactor::Color;
                if (has != (i < w)) return false;
                if (w < 3) return false;
            }
        }
        /* 常量因子：第 j 个操作数 = 各分量的第 j 个常量因子（没有的分量补 1） */
        size_t nk = 0;
        for (int i = 0; i < 4; i++) {
            size_t c = 0;
            for (const auto &f : lf[i]) c += f.kind == LaneFactor::Konst;
            nk = std::max(nk, c);
        }
        Scalar one;
        bool have_one = false;
        for (size_t j = 0; j < nk; j++) {
            Operand op;
            op.n = 4;
            for (int i = 0; i < 4; i++) {
                size_t c = 0;
                bool set = false;
                for (const auto &f : lf[i]) {
                    if (f.kind != LaneFactor::Konst) continue;
                    if (c++ == j) {
                        op.s[i] = f.k;
                        set = true;
                    }
                }
                if (!set) {
                    if (!have_one) {
                        /* 找一个值为 1 的常量节点（没有就放弃：极少见） */
                        for (size_t id = 0; id < d.nodes.size() && !have_one; id++) {
                            if (d.nodes[id].kind == NK::Const && d.nodes[id].c == 1.0f) {
                                one = {static_cast<int>(id), 0};
                                have_one = true;
                            }
                        }
                        if (!have_one) return false;
                    }
                    op.s[i] = one;
                }
            }
            fs.konst.push_back(op);
        }
        return true;
    }

    bool try_pattern()
    {
        if (!d.has_color) return false;
        Factors f;
        if (!factors(d.color, f)) return false;
        if (f.tex.size() > 2) return false;

        /* alpha 测试：唯一的 KIL，条件是输出 alpha 与常量比较 */
        bool at = false;
        uint8_t func = ESPGPU_CMP_ALWAYS, ref = 0;
        if (d.kills.size() > 1) return false;
        if (d.kills.size() == 1) {
            const Node &k = N(d.kills[0]);
            const Scalar p = k.a[0].s[0];
            const Node &c = N(p.node);
            if (k.a[0].neg || c.kind != NK::Op || c.a[0].neg || c.a[1].neg ||
                (c.op != ESPGPU_VM_SLT && c.op != ESPGPU_VM_SGE)) {
                return false;
            }
            const Scalar x = c.a[0].s[p.comp], y = c.a[1].s[p.comp];
            const Scalar alpha = d.color.s[3];
            float kv;
            if (x == alpha && isK(y, &kv)) {
                alpha_ref_for(c.op, true, kv, func, ref);
            } else if (y == alpha && isK(x, &kv)) {
                alpha_ref_for(c.op, false, kv, func, ref);
            } else {
                return false;
            }
            at = true;
        }

        FragmentPlan fp;
        fp.program = false;
        fp.alpha_test = at;
        fp.alpha_func = func;
        fp.alpha_ref = ref;
        if (f.color >= 0) {
            fp.color_input = f.color;
            fp.varying_slots.push_back({f.color, ESPGPU_ATTR_COLOR});
        }
        for (const auto &k : f.konst) {
            bool white = true;
            for (int i = 0; i < 4; i++) white = white && isK(k.s[i], 1.0f);
            if (!white) fp.color_slots.push_back(konst_slot(k));
        }
        for (size_t i = 0; i < f.tex.size(); i++) {
            fp.varying_slots.push_back({f.tex[i].second, static_cast<int>(ESPGPU_ATTR_TEXCOORD0 + i)});
            fp.unit_sampler.push_back(d.samplers[static_cast<size_t>(f.tex[i].first)]);
        }
        const bool no_color = f.color < 0 && fp.color_slots.empty();
        if (f.tex.empty()) {
            fp.shade_mode = ESPGPU_SHADE_FLAT; /* 颜色全是常量白时也是 FLAT + 白 */
            fp.color_white = no_color;
        } else if (f.tex.size() == 1) {
            fp.shade_mode = no_color ? ESPGPU_SHADE_TEX : ESPGPU_SHADE_TEX_MOD_COLOR;
        } else {
            fp.shade_mode = ESPGPU_SHADE_TEX_MOD_TEX;
            fp.color_white = no_color;
        }
        sh.frag = fp;
        return finalize_consts();
    }

    /* ── 窥孔改写（只改 VM 要算的节点；uo 节点在主机上算，不花指令） ── */

    void fold_neg(Operand &o)
    {
        for (;;) {
            if (o.n == 0) return;
            Scalar ns[4];
            for (int i = 0; i < o.n; i++) {
                const Node &m = N(o.s[i].node);
                if (m.kind != NK::Op || m.op != ESPGPU_VM_SUB || m.a[0].neg || m.a[1].neg) return;
                if (!isK(m.a[0].s[o.s[i].comp], 0.0f)) return;
                ns[i] = m.a[1].s[o.s[i].comp];
            }
            for (int i = 0; i < o.n; i++) o.s[i] = ns[i];
            o.neg ^= 1;
        }
    }

    void rewrite_neg()
    {
        for (int id : live) {
            if (!emits(id)) continue;
            Node &n = NM(id);
            for (int k = 0; k < n.nargs; k++) fold_neg(n.a[k]);
        }
    }

    void fuse_mad()
    {
        for (int id : live) {
            Node &n = NM(id);
            if (n.kind != NK::Op || !emits(id) || (n.op != ESPGPU_VM_ADD && n.op != ESPGPU_VM_SUB)) continue;
            for (int k = 1; k >= 0; k--) {
                const Operand o = n.a[k];
                const int m = single_node(o);
                if (m < 0) continue;
                const Node &mn = N(m);
                if (mn.kind != NK::Op || mn.op != ESPGPU_VM_MUL || !emits(m) || uses[static_cast<size_t>(m)] != 1) continue;
                Operand f0 = compose(mn.a[0], o), f1 = compose(mn.a[1], o);
                Operand other = n.a[1 - k];
                bool neg_m = o.neg != 0;
                if (n.op == ESPGPU_VM_SUB && k == 1) neg_m = !neg_m;   /* x - M */
                if (n.op == ESPGPU_VM_SUB && k == 0) other.neg ^= 1;  /* M - y = M + (-y) */
                if (neg_m) f0.neg ^= 1;
                n.op = ESPGPU_VM_MAD;
                n.nargs = 3;
                n.a[0] = f0;
                n.a[1] = f1;
                n.a[2] = other;
                uses[static_cast<size_t>(m)] = 0;
                break;
            }
        }
    }

    /** P = SLT/SGE(x, y) 且 o 用到的分量里有一侧全是 0：返回另一侧（必要时取负）与比较方向 */
    bool cmp_zero(const Operand &o, int p, Operand &x, bool &lt) const
    {
        const Node &pn = N(p);
        if (pn.kind != NK::Op || (pn.op != ESPGPU_VM_SLT && pn.op != ESPGPU_VM_SGE)) return false;
        bool z1 = true, z0 = true;
        for (int i = 0; i < o.n; i++) {
            z1 = z1 && isK(pn.a[1].s[std::min<int>(o.s[i].comp, pn.a[1].n - 1)], 0.0f);
            z0 = z0 && isK(pn.a[0].s[std::min<int>(o.s[i].comp, pn.a[0].n - 1)], 0.0f);
        }
        if (z1) {
            /* x < 0  /  x >= 0 */
            x = compose(pn.a[0], o);
            lt = pn.op == ESPGPU_VM_SLT;
            return true;
        }
        if (z0) {
            /* 0 < y ⟺ -y < 0；0 >= y ⟺ !(-y < 0) */
            x = compose(pn.a[1], o);
            x.neg ^= 1;
            lt = pn.op == ESPGPU_VM_SLT;
            return true;
        }
        return false;
    }

    void sel_cmp()
    {
        for (int id : live) {
            Node &n = NM(id);
            if (n.kind != NK::Op || n.op != ESPGPU_VM_SEL || !emits(id)) continue;
            const int p = single_node(n.a[0]);
            if (p < 0 || !emits(p) || uses[static_cast<size_t>(p)] != 1) continue;
            Operand x;
            bool lt;
            if (!cmp_zero(n.a[0], p, x, lt)) continue;
            /* SEL(x<0, a, b) = CMP(x, a, b)；SEL(x>=0, a, b) = CMP(x, b, a) */
            n.op = ESPGPU_VM_CMP;
            n.a[0] = x;
            if (!lt) std::swap(n.a[1], n.a[2]);
            uses[static_cast<size_t>(p)] = 0;
        }
    }

    void kill_direct()
    {
        for (int k : d.kills) {
            Node &kn = NM(k);
            if (kn.index != 0 || kn.a[0].neg) continue;
            const int p = single_node(kn.a[0]);
            if (p < 0 || !emits(p) || uses[static_cast<size_t>(p)] != 1) continue;
            Operand x;
            bool lt;
            if (!cmp_zero(kn.a[0], p, x, lt) || !lt) continue; /* 只有严格 < 0 能直接交给 KIL */
            kn.a[0] = x;
            kn.index = 1;
            uses[static_cast<size_t>(p)] = 0;
        }
    }

    /* ── 只依赖 uniform 的系数重结合（要求主机预计算） ── */

    /** 新建一个 uo 运算节点（主机预计算，编号大于所有现有节点，不影响 VM 发射顺序） */
    Operand new_uo(uint8_t op, const Operand &a, const Operand &b, const Operand *c = nullptr)
    {
        Node nd;
        nd.kind = NK::Op;
        nd.op = op;
        nd.width = a.n;
        nd.nargs = c ? 3 : 2;
        nd.a[0] = a;
        nd.a[1] = b;
        if (c) nd.a[2] = *c;
        d.nodes.push_back(nd);
        uo.push_back(1);
        uses.push_back(0);
        lanes.push_back(0);
        Operand r;
        r.n = a.n;
        for (int i = 0; i < a.n; i++) r.s[i] = {static_cast<int>(d.nodes.size()) - 1, static_cast<uint8_t>(i)};
        return r;
    }

    /** value = x * m + a：x 是 VM 要算的操作数，m / a 只依赖 uniform（hm / ha = false 表示 1 / 0） */
    struct Aff {
        Operand x, m, a;
        bool hm = false, ha = false;
        int node = -1;
    };

    bool aff_of(const Operand &o, Aff &f) const
    {
        const int id = single_node(o);
        if (id < 0 || !emits(id) || uses[static_cast<size_t>(id)] != 1) return false;
        const Node &n = N(id);
        if (n.kind != NK::Op || n.flags) return false;
        auto u = [&](int k) { return all_uo(n.a[k]); };
        f = Aff{};
        f.node = id;
        switch (n.op) {
        case ESPGPU_VM_MUL:
            if (u(1) && !u(0)) { f.x = n.a[0]; f.m = n.a[1]; }
            else if (u(0) && !u(1)) { f.x = n.a[1]; f.m = n.a[0]; }
            else return false;
            f.hm = true;
            break;
        case ESPGPU_VM_ADD:
            if (u(1) && !u(0)) { f.x = n.a[0]; f.a = n.a[1]; }
            else if (u(0) && !u(1)) { f.x = n.a[1]; f.a = n.a[0]; }
            else return false;
            f.ha = true;
            break;
        case ESPGPU_VM_SUB:
            if (u(1) && !u(0)) { f.x = n.a[0]; f.a = n.a[1]; f.a.neg ^= 1; }
            else if (u(0) && !u(1)) { f.x = n.a[1]; f.x.neg ^= 1; f.a = n.a[0]; }
            else return false;
            f.ha = true;
            break;
        case ESPGPU_VM_MAD:
            if (!u(2)) return false;
            if (u(1) && !u(0)) { f.x = n.a[0]; f.m = n.a[1]; }
            else if (u(0) && !u(1)) { f.x = n.a[1]; f.m = n.a[0]; }
            else return false;
            f.a = n.a[2];
            f.hm = f.ha = true;
            break;
        default: return false;
        }
        f.x = compose(f.x, o);
        if (f.hm) f.m = compose(f.m, o);
        if (f.ha) f.a = compose(f.a, o);
        if (o.neg) {
            f.x.neg ^= 1;
            if (f.ha) f.a.neg ^= 1;
        }
        return true;
    }

    void set_aff(Node &t, const Aff &f)
    {
        if (f.hm && f.ha) {
            t.op = ESPGPU_VM_MAD;
            t.nargs = 3;
            t.a[0] = f.x;
            t.a[1] = f.m;
            t.a[2] = f.a;
        } else if (f.hm) {
            t.op = ESPGPU_VM_MUL;
            t.nargs = 2;
            t.a[0] = f.x;
            t.a[1] = f.m;
        } else if (f.ha) {
            t.op = ESPGPU_VM_ADD;
            t.nargs = 2;
            t.a[0] = f.x;
            t.a[1] = f.a;
        }
        uses[static_cast<size_t>(f.node)] = 0;
    }

    /** (x·m + a) OP k，k 只依赖 uniform → 一条 x·m' + a'（m'、a' 在主机上算） */
    void reassoc()
    {
        const size_t n0 = d.nodes.size();
        for (size_t id = 0; id < n0; id++) {
            if (!emits(static_cast<int>(id)) || uses[id] == 0) continue;
            Node &t = d.nodes[id];
            if (t.kind != NK::Op || t.flags) continue;
            if (t.op == ESPGPU_VM_DIV && all_uo(t.a[1]) && !all_uo(t.a[0])) {
                /* x / k → x · (1/k)：倒数在主机上算（DIV 与设备一样 k = 0 时得 0） */
                Operand one = t.a[1];
                one.neg = 0;
                Scalar k1;
                if (!find_const(1.0f, k1)) continue;
                for (int i = 0; i < one.n; i++) one.s[i] = k1;
                const Operand r = new_uo(ESPGPU_VM_DIV, one, t.a[1]);
                Node &tt = d.nodes[id];
                tt.op = ESPGPU_VM_MUL;
                tt.a[1] = r;
            }
            Node &tn = d.nodes[id];
            if (tn.nargs != 2) continue;
            for (int side = 0; side < 2; side++) {
                const Operand ka = tn.a[1 - side];
                if (!all_uo(ka) || all_uo(tn.a[side])) continue;
                Aff f;
                if (!aff_of(tn.a[side], f)) continue;
                if (tn.op == ESPGPU_VM_MUL) {
                    f.m = f.hm ? new_uo(ESPGPU_VM_MUL, f.m, ka) : ka;
                    f.hm = true;
                    if (f.ha) f.a = new_uo(ESPGPU_VM_MUL, f.a, ka);
                } else if (tn.op == ESPGPU_VM_ADD || (tn.op == ESPGPU_VM_SUB && side == 0)) {
                    Operand k2 = ka;
                    if (tn.op == ESPGPU_VM_SUB) k2.neg ^= 1;
                    f.a = f.ha ? new_uo(ESPGPU_VM_ADD, f.a, k2) : k2;
                    f.ha = true;
                } else if (tn.op == ESPGPU_VM_SUB && side == 1) {
                    /* k - (x·m + a) = (-x)·m + (k - a) */
                    f.x.neg ^= 1;
                    if (f.ha) {
                        Operand na = f.a;
                        na.neg ^= 1;
                        f.a = new_uo(ESPGPU_VM_ADD, ka, na);
                    } else {
                        f.a = ka;
                    }
                    f.ha = true;
                } else {
                    continue;
                }
                set_aff(d.nodes[id], f);
                break;
            }
        }
    }

    bool find_const(float v, Scalar &out) const
    {
        for (size_t id = 0; id < d.nodes.size(); id++) {
            if (d.nodes[id].kind == NK::Const && d.nodes[id].c == v) {
                out = {static_cast<int>(id), 0};
                return true;
            }
        }
        return false;
    }

    /** 1 - smoothstep(a, b, x) → smoothstep(b, a, x)（a == b 时 GLSL 未定义） */
    void smooth_flip()
    {
        for (int id : live) {
            Node &n = NM(id);
            if (n.kind != NK::Op || n.op != ESPGPU_VM_SUB || !emits(id) || n.flags || n.a[0].neg || n.a[1].neg) continue;
            bool one = true;
            for (int i = 0; i < n.a[0].n; i++) one = one && isK(n.a[0].s[i], 1.0f);
            const int x = single_node(n.a[1]);
            if (!one || x < 0 || !emits(x) || uses[static_cast<size_t>(x)] != 1) continue;
            const Node sm = N(x);
            if (sm.kind != NK::Op || sm.op != ESPGPU_VM_SMOOTH || sm.flags) continue;
            const Operand sel = n.a[1];
            n.op = ESPGPU_VM_SMOOTH;
            n.nargs = 3;
            n.a[0] = compose(sm.a[1], sel);
            n.a[1] = compose(sm.a[0], sel);
            n.a[2] = compose(sm.a[2], sel);
            uses[static_cast<size_t>(x)] = 0;
        }
    }

    /** clamp(X, 0, 1) → X 带 SAT 修饰 */
    void sat_fold()
    {
        for (int id : live) {
            Node &c = NM(id);
            if (c.kind != NK::Op || c.op != ESPGPU_VM_CLAMP || !emits(id) || c.a[0].neg) continue;
            bool z = true, o1 = true;
            for (int i = 0; i < c.a[1].n; i++) z = z && isK(c.a[1].s[i], c.a[1].neg ? -0.0f : 0.0f);
            for (int i = 0; i < c.a[2].n; i++) o1 = o1 && isK(c.a[2].s[i], c.a[2].neg ? -1.0f : 1.0f);
            if (!z || !o1) continue;
            const int x = single_node(c.a[0]);
            if (x < 0 || !emits(x) || uses[static_cast<size_t>(x)] != 1) continue;
            const Node xn = N(x);
            if (xn.kind != NK::Op) continue;
            const Operand sel = c.a[0];
            c.op = xn.op;
            c.nargs = xn.nargs;
            for (int k = 0; k < xn.nargs; k++) c.a[k] = componentwise(xn.op) ? compose(xn.a[k], sel) : xn.a[k];
            c.flags = static_cast<uint8_t>(xn.flags | ESPGPU_VM_F_SAT);
            uses[static_cast<size_t>(x)] = 0;
        }
    }

    /** 自底向上 SLP：同一个操作数里来自几个同构标量运算的分量合并成一条向量运算 */
    void slp(Operand &o, int depth)
    {
        if (depth > 32 || o.n < 2) return;
        auto mergeable = [&](int x) {
            const Node &n = N(x);
            return emits(x) && n.kind == NK::Op && componentwise(n.op) && n.width == 1 &&
                   uses[static_cast<size_t>(x)] == 1;
        };
        bool done[4] = {false, false, false, false};
        for (int i = 0; i < o.n; i++) {
            if (done[i] || !mergeable(o.s[i].node)) continue;
            const Node &a = N(o.s[i].node);
            std::vector<int> members = {o.s[i].node};
            for (int j = i + 1; j < o.n; j++) {
                const int x = o.s[j].node;
                if (done[j] || !mergeable(x) || std::find(members.begin(), members.end(), x) != members.end()) continue;
                const Node &b = N(x);
                bool same = b.op == a.op && b.nargs == a.nargs && b.flags == a.flags;
                for (int k = 0; k < a.nargs && same; k++) same = b.a[k].neg == a.a[k].neg;
                if (same) members.push_back(x);
            }
            if (members.size() < 2) continue;
            const int m = *std::max_element(members.begin(), members.end());
            Node merged = N(m);
            merged.width = static_cast<uint8_t>(members.size());
            for (int k = 0; k < merged.nargs; k++) {
                merged.a[k].n = static_cast<uint8_t>(members.size());
                for (size_t q = 0; q < members.size(); q++) merged.a[k].s[q] = N(members[q]).a[k].s[0];
            }
            for (int x : members) uses[static_cast<size_t>(x)] = 0;
            NM(m) = merged;
            uses[static_cast<size_t>(m)] = 1;
            for (int j = 0; j < o.n; j++) {
                const auto it = std::find(members.begin(), members.end(), o.s[j].node);
                if (it == members.end()) continue;
                o.s[j] = {m, static_cast<uint8_t>(it - members.begin())};
                done[j] = true;
            }
            for (int k = 0; k < merged.nargs; k++) slp(NM(m).a[k], depth + 1);
        }
    }

    void slp_all()
    {
        if (d.has_color) slp(d.color, 0);
        for (auto it = live.rbegin(); it != live.rend(); ++it) {
            const int id = *it;
            if (!emits(id) || (uses[static_cast<size_t>(id)] == 0 && N(id).kind != NK::Kill)) continue;
            for (int k = 0; k < N(id).nargs; k++) slp(NM(id).a[k], 0);
        }
    }

    /* ── 寄存器（按分量）分配 ── */

    int free_lanes(int r, int gid) const
    {
        int n = 0;
        for (int l = 0; l < 4; l++) n += owner[r][l] < 0 && (resv[r][l] < 0 || resv[r][l] == gid);
        return n;
    }

    /** 分配 need 个分量；identity 非 0 时必须正好是这些分量（TEX） */
    bool alloc_lanes(int node, int need, unsigned identity, int gid, Place &pl)
    {
        int best = -1, best_free = 99;
        if (identity) {
            for (int r = 0; r < static_cast<int>(ESPGPU_VM_TEMPS); r++) {
                bool ok = true;
                for (int l = 0; l < 4; l++) {
                    if (identity & (1u << l)) ok = ok && owner[r][l] < 0 && resv[r][l] < 0;
                }
                const int fr = free_lanes(r, -1);
                if (ok && fr < best_free) {
                    best = r;
                    best_free = fr;
                }
            }
            if (best < 0) return fail("fragment shader needs more than 16 live temporaries");
            pl.reg = static_cast<uint8_t>(best);
            for (int l = 0; l < 4; l++) {
                if (identity & (1u << l)) {
                    owner[best][l] = node;
                    pl.lane[l] = static_cast<int8_t>(l);
                }
            }
            return true;
        }
        if (gid >= 0) {
            Group &g = groups[static_cast<size_t>(gid)];
            if (g.reg < 0) {
                for (int r = 0; r < static_cast<int>(ESPGPU_VM_TEMPS); r++) {
                    const int fr = free_lanes(r, -1);
                    if (fr >= g.lanes && fr < best_free) {
                        best = r;
                        best_free = fr;
                    }
                }
                if (best >= 0) {
                    int left = g.lanes;
                    for (int l = 0; l < 4 && left > 0; l++) {
                        if (owner[best][l] < 0 && resv[best][l] < 0) {
                            resv[best][l] = gid;
                            left--;
                        }
                    }
                    g.reg = best;
                }
            }
            if (g.reg >= 0 && free_lanes(g.reg, gid) >= need) {
                best = g.reg;
            } else {
                gid = -1;
                best = -1;
            }
        }
        if (best < 0) {
            best_free = 99;
            for (int r = 0; r < static_cast<int>(ESPGPU_VM_TEMPS); r++) {
                const int fr = free_lanes(r, -1);
                if (fr >= need && fr < best_free) {
                    best = r;
                    best_free = fr;
                }
            }
        }
        if (best < 0) return fail("fragment shader needs more than 16 live temporaries");
        pl.reg = static_cast<uint8_t>(best);
        int logical = 0;
        /* 先用给本组预留的分量，再用空闲分量 */
        for (int pass = gid >= 0 ? 0 : 1; pass < 2; pass++) {
            for (int l = 0; l < 4 && need > 0; l++) {
                const bool ok = owner[best][l] < 0 && (pass == 0 ? resv[best][l] == gid : resv[best][l] < 0);
                if (!ok) continue;
                owner[best][l] = node;
                resv[best][l] = -1;
                while (logical < 4 && pl.lane[logical] != -2) logical++;
                if (logical < 4) pl.lane[logical] = static_cast<int8_t>(l);
                need--;
            }
        }
        return true;
    }

    void free_node(int node)
    {
        for (auto &r : owner)
            for (int &x : r) {
                if (x == node) x = -1;
            }
    }

    void release(int node)
    {
        int &u = uses[static_cast<size_t>(node)];
        if (--u == 0 && emits(node) && N(node).kind != NK::Kill && out_mask[static_cast<size_t>(node)] == 0) {
            free_node(node);
        }
    }

    void release_operand(const Operand &o)
    {
        int seen[4];
        int ns = 0;
        for (int i = 0; i < o.n; i++) {
            bool dup = false;
            for (int k = 0; k < ns; k++) dup = dup || seen[k] == o.s[i].node;
            if (!dup) {
                seen[ns++] = o.s[i].node;
                release(o.s[i].node);
            }
        }
    }

    /* ── 操作数解析 ── */

    struct Src {
        uint8_t reg = ESPGPU_VM_ZERO;
        uint8_t comp[4] = {0, 1, 2, 3}; /* 逻辑分量 → 寄存器分量 */
        int n = 1;
        bool neg = false;
    };

    /** 标量所在寄存器；常量返回 false。主机预计算的标量按需放进扩展槽 */
    bool where(const Scalar &s, uint8_t &reg, int &comp)
    {
        const Node &n = N(s.node);
        comp = s.comp;
        if (pre(s.node)) {
            int lo[4];
            const int slot = xplace({lane_of_scalar(s)}, lo);
            reg = xreg(slot);
            comp = lo[0];
            return true;
        }
        switch (n.kind) {
        case NK::Op:
        case NK::Tex: {
            const Place &p = place[static_cast<size_t>(s.node)];
            reg = ESPGPU_VM_REG(ESPGPU_VM_BANK_TEMP, p.reg);
            comp = p.lane[s.comp];
            return true;
        }
        case NK::Varying:
            reg = ESPGPU_VM_REG(ESPGPU_VM_BANK_VARYING, vreg_of_input[static_cast<size_t>(n.index)]);
            return true;
        case NK::FragCoord: reg = ESPGPU_VM_REG(ESPGPU_VM_BANK_VARYING, 6); return true;
        case NK::Uniform: reg = ESPGPU_VM_REG(ESPGPU_VM_BANK_CONST, n.index); return true;
        default: return false;
        }
    }

    /** 一组常量：全 0 / 全 1 走特殊寄存器，否则进常量槽 */
    void const_src(const std::vector<float> &vals, uint8_t &reg, int comp_out[4])
    {
        bool zero = true, one = true;
        for (float v : vals) {
            zero = zero && v == 0.0f;
            one = one && v == 1.0f;
        }
        if (zero || one) {
            reg = zero ? ESPGPU_VM_ZERO : ESPGPU_VM_ONE;
            for (size_t i = 0; i < vals.size(); i++) comp_out[i] = 0;
            return;
        }
        std::vector<PreLane> want;
        for (float v : vals) {
            PreLane l;
            l.k = v == 0.0f ? 0.0f : v;
            want.push_back(l);
        }
        reg = xreg(xplace(want, comp_out));
    }

    void emit_raw(uint8_t op, uint8_t dst, uint8_t mask, const uint8_t src[3], const uint8_t swz[3], uint8_t neg,
                  uint8_t unit = 0)
    {
        espgpu_vm_insn_t in{};
        in.op = op;
        in.dst = dst;
        in.mask = mask;
        in.flags = 0;
        for (int k = 0; k < 3; k++) {
            in.src[k] = src[k];
            in.swz[k] = swz[k];
        }
        in.neg = neg;
        in.tex_unit = unit;
        sh.frag.code.push_back(in);
    }

    /** 用 MOV 把 o 的分量拼进 dst 寄存器的 lane[i] 分量（dst 可以是 o0） */
    void assemble(const Operand &o, const std::vector<int> &idx, uint8_t dst, const int lane[4])
    {
        std::vector<char> done(idx.size(), 0);
        for (size_t a = 0; a < idx.size(); a++) {
            if (done[a]) continue;
            uint8_t ra;
            int ca;
            const bool reg_a = where(o.s[idx[a]], ra, ca);
            int sw[4] = {0, 0, 0, 0};
            uint8_t mask = 0;
            uint8_t sreg;
            if (reg_a) {
                for (size_t b = a; b < idx.size(); b++) {
                    uint8_t rb;
                    int cb;
                    if (!done[b] && where(o.s[idx[b]], rb, cb) && rb == ra) {
                        sw[lane[idx[b]]] = cb;
                        mask |= static_cast<uint8_t>(1u << lane[idx[b]]);
                        done[b] = 1;
                    }
                }
                sreg = ra;
            } else {
                std::vector<float> vals;
                std::vector<size_t> which;
                for (size_t b = a; b < idx.size(); b++) {
                    uint8_t rb;
                    int cb;
                    if (!done[b] && !where(o.s[idx[b]], rb, cb)) {
                        vals.push_back(N(o.s[idx[b]].node).c);
                        which.push_back(b);
                        done[b] = 1;
                    }
                }
                int cc[4];
                const_src(vals, sreg, cc);
                for (size_t q = 0; q < which.size(); q++) {
                    sw[lane[idx[which[q]]]] = cc[q];
                    mask |= static_cast<uint8_t>(1u << lane[idx[which[q]]]);
                }
            }
            /* 掩码外的分量也指向有效分量 */
            int first = 0;
            for (int l = 0; l < 4; l++) {
                if (mask & (1u << l)) {
                    first = sw[l];
                    break;
                }
            }
            for (int l = 0; l < 4; l++) {
                if (!(mask & (1u << l))) sw[l] = first;
            }
            const uint8_t src[3] = {sreg, ESPGPU_VM_ZERO, ESPGPU_VM_ZERO};
            const uint8_t swz[3] = {ESPGPU_VM_SWZ(sw[0], sw[1], sw[2], sw[3]), ESPGPU_VM_SWZ_IDENTITY,
                                    ESPGPU_VM_SWZ_IDENTITY};
            emit_raw(ESPGPU_VM_MOV, dst, mask, src, swz, o.neg ? 1 : 0);
        }
    }

    /** 解析操作数；需要拼装时 tmp 返回占用的临时节点号（调用方释放），否则 -1 */
    bool resolve(const Operand &o, Src &out, int &tmp)
    {
        tmp = -1;
        out = Src{};
        out.n = o.n;
        out.neg = o.neg != 0;
        const int n = o.n;
        uint8_t regs[4];
        int comps[4];
        bool is_reg[4];
        bool all_const = true;

        /* 全部只依赖 uniform：整个操作数放进一个扩展槽（不必拼装） */
        bool has_pre = false;
        for (int i = 0; i < n; i++) has_pre = has_pre || pre(o.s[i].node);
        if (use_pre && all_uo(o)) {
            bool same = true;
            for (int i = 0; i < n; i++) {
                is_reg[i] = where_nopre(o.s[i], regs[i], comps[i]);
                all_const = all_const && !is_reg[i];
                same = same && is_reg[i] && regs[i] == regs[0];
            }
            if (!all_const && (!same || has_pre)) {
                std::vector<PreLane> want;
                for (int i = 0; i < n; i++) want.push_back(lane_of_scalar(o.s[i]));
                int lo[4];
                out.reg = xreg(xplace(want, lo));
                for (int i = 0; i < n; i++) out.comp[i] = static_cast<uint8_t>(lo[i]);
                return true;
            }
        } else if (has_pre) {
            /* 混合操作数：里面的预计算标量尽量放进同一个槽 */
            std::vector<PreLane> want;
            for (int i = 0; i < n; i++) {
                if (pre(o.s[i].node)) want.push_back(lane_of_scalar(o.s[i]));
            }
            int lo[4];
            xplace(want, lo);
        }
        all_const = true;
        for (int i = 0; i < n; i++) {
            is_reg[i] = where(o.s[i], regs[i], comps[i]);
            all_const = all_const && !is_reg[i];
        }
        if (all_const) {
            std::vector<float> vals;
            for (int i = 0; i < n; i++) vals.push_back(N(o.s[i].node).c);
            int cc[4];
            const_src(vals, out.reg, cc);
            for (int i = 0; i < n; i++) out.comp[i] = static_cast<uint8_t>(cc[i]);
            return true;
        }
        bool same = true;
        for (int i = 0; i < n; i++) same = same && is_reg[i] && regs[i] == regs[0];
        if (same) {
            out.reg = regs[0];
            for (int i = 0; i < n; i++) out.comp[i] = static_cast<uint8_t>(comps[i]);
            return true;
        }

        /* 拼装进临时寄存器的 n 个分量 */
        tmp = static_cast<int>(d.nodes.size()) + 1; /* 伪节点号 */
        Place pl;
        for (int i = 0; i < n; i++) pl.lane[i] = -2;
        if (!alloc_lanes(tmp, n, 0, -1, pl)) return false;
        std::vector<int> idx;
        int lane[4];
        for (int i = 0; i < n; i++) {
            idx.push_back(i);
            lane[i] = pl.lane[i];
        }
        Operand pos = o;
        pos.neg = 0;
        assemble(pos, idx, ESPGPU_VM_REG(ESPGPU_VM_BANK_TEMP, pl.reg), lane);
        out.reg = ESPGPU_VM_REG(ESPGPU_VM_BANK_TEMP, pl.reg);
        for (int i = 0; i < n; i++) out.comp[i] = static_cast<uint8_t>(pl.lane[i]);
        return true;
    }

    /** where() 但不为预计算标量分配槽（只用于判断） */
    bool where_nopre(const Scalar &s, uint8_t &reg, int &comp) const
    {
        const Node &n = N(s.node);
        comp = s.comp;
        if (n.kind == NK::Uniform) {
            reg = ESPGPU_VM_REG(ESPGPU_VM_BANK_CONST, n.index);
            return true;
        }
        if (n.kind == NK::Const) return false;
        reg = 0xFF; /* 预计算标量：各算各的，视为不同寄存器 */
        comp = 0;
        return true;
    }

    static uint8_t swz_of(const Src &s, const int8_t logical_of_lane[4])
    {
        int c[4];
        for (int l = 0; l < 4; l++) {
            const int lg = std::min<int>(std::max<int>(logical_of_lane[l], 0), s.n - 1);
            c[l] = s.comp[lg];
        }
        return ESPGPU_VM_SWZ(c[0], c[1], c[2], c[3]);
    }

    static uint8_t swz_seq(const Src &s)
    {
        int c[4];
        for (int l = 0; l < 4; l++) c[l] = s.comp[std::min(l, s.n - 1)];
        return ESPGPU_VM_SWZ(c[0], c[1], c[2], c[3]);
    }

    /* ── 发射 ── */

    void plan_outputs()
    {
        out_mask.assign(d.nodes.size(), 0);
        out_map.assign(d.nodes.size(), {-1, -1, -1, -1});
        if (!d.has_color || d.color.neg) return;
        for (int i = 0; i < d.color.n; i++) {
            const int x = d.color.s[i].node;
            const Node &n = N(x);
            if (!emits(x) || n.kind == NK::Kill || uses[static_cast<size_t>(x)] != 1) continue;
            const int c = d.color.s[i].comp;
            if (n.kind == NK::Tex && c != i) {
                out_mask[static_cast<size_t>(x)] = 0xF0; /* 标记为不可直写 */
                continue;
            }
            if (out_mask[static_cast<size_t>(x)] & 0xF0) continue;
            out_mask[static_cast<size_t>(x)] |= static_cast<uint8_t>(1u << i);
            out_map[static_cast<size_t>(x)][static_cast<size_t>(i)] = static_cast<int8_t>(c);
        }
        for (auto &m : out_mask) {
            if (m & 0xF0) m = 0;
        }
    }

    void plan_groups()
    {
        group_of.assign(d.nodes.size(), -1);
        groups.clear();
        if (!use_pack) return;
        auto consider = [&](const Operand &o) {
            std::vector<int> t;
            for (int i = 0; i < o.n; i++) {
                const int x = o.s[i].node;
                const Node &n = N(x);
                if (!emits(x) || n.kind != NK::Op || out_mask[static_cast<size_t>(x)]) continue;
                if (std::find(t.begin(), t.end(), x) == t.end()) t.push_back(x);
            }
            if (t.size() < 2) return;
            int gid = -1, total = 0;
            for (int x : t) {
                const int g = group_of[static_cast<size_t>(x)];
                if (g >= 0) {
                    if (gid >= 0 && gid != g) return;
                    gid = g;
                } else {
                    total += popcount4(lanes[static_cast<size_t>(x)]);
                }
            }
            if (gid >= 0) total += groups[static_cast<size_t>(gid)].lanes;
            if (total > 4) return;
            if (gid < 0) {
                groups.push_back({});
                gid = static_cast<int>(groups.size()) - 1;
            }
            for (int x : t) {
                if (group_of[static_cast<size_t>(x)] < 0) {
                    group_of[static_cast<size_t>(x)] = gid;
                    groups[static_cast<size_t>(gid)].lanes += popcount4(lanes[static_cast<size_t>(x)]);
                }
            }
        };
        for (int id : live) {
            if (!emits(id)) continue;
            const Node &n = N(id);
            for (int k = 0; k < n.nargs; k++) consider(n.a[k]);
        }
        if (d.has_color) consider(d.color);
    }

    /**
     * 发射顺序：从根（先各个 KIL——越早丢弃越省——再输出颜色）做后序 DFS，
     * 子树按 Sethi-Ullman 需求从大到小访问。比按源码顺序发射少占很多寄存器
     * （例如先算出一串局部变量、最后才乘起来的写法）。
     */
    std::vector<int> schedule()
    {
        const size_t n = d.nodes.size();
        std::vector<int> need(n, 0);
        for (int id : live) {
            if (!emits(id)) continue;
            const Node &nd = N(id);
            std::vector<int> c;
            for (int k = 0; k < nd.nargs; k++)
                for (int i = 0; i < nd.a[k].n; i++) c.push_back(need[static_cast<size_t>(nd.a[k].s[i].node)]);
            std::sort(c.rbegin(), c.rend());
            int m = 1;
            for (size_t i = 0; i < c.size(); i++) m = std::max(m, c[i] + static_cast<int>(i));
            need[static_cast<size_t>(id)] = m;
        }
        std::vector<int> order;
        std::vector<char> state(n, 0); /* 0 未访问 1 展开中 2 已输出 */
        auto visit = [&](int root) {
            std::vector<std::pair<int, size_t>> st; /* (节点, 下一个要访问的孩子) */
            std::vector<std::vector<int>> kids_of;
            auto kids = [&](int id) {
                std::vector<int> k;
                const Node &nd = N(id);
                for (int a = 0; a < nd.nargs; a++)
                    for (int i = 0; i < nd.a[a].n; i++) {
                        const int c = nd.a[a].s[i].node;
                        if (emits(c) && state[static_cast<size_t>(c)] == 0 &&
                            std::find(k.begin(), k.end(), c) == k.end()) {
                            k.push_back(c);
                        }
                    }
                std::stable_sort(k.begin(), k.end(), [&](int x, int y) {
                    return need[static_cast<size_t>(x)] > need[static_cast<size_t>(y)];
                });
                return k;
            };
            if (!emits(root) || state[static_cast<size_t>(root)]) return;
            state[static_cast<size_t>(root)] = 1;
            st.push_back({root, 0});
            kids_of.push_back(kids(root));
            while (!st.empty()) {
                auto &top = st.back();
                const std::vector<int> &ks = kids_of.back();
                if (top.second < ks.size()) {
                    const int c = ks[top.second++];
                    if (state[static_cast<size_t>(c)]) continue;
                    state[static_cast<size_t>(c)] = 1;
                    st.push_back({c, 0});
                    kids_of.push_back(kids(c));
                    continue;
                }
                state[static_cast<size_t>(top.first)] = 2;
                order.push_back(top.first);
                st.pop_back();
                kids_of.pop_back();
            }
        };
        for (int k : d.kills) visit(k);
        if (d.has_color) {
            /* 颜色的各分量也按需求从大到小 */
            std::vector<int> roots;
            for (int i = 0; i < d.color.n; i++) {
                const int c = d.color.s[i].node;
                if (std::find(roots.begin(), roots.end(), c) == roots.end()) roots.push_back(c);
            }
            std::stable_sort(roots.begin(), roots.end(), [&](int x, int y) {
                return need[static_cast<size_t>(x)] > need[static_cast<size_t>(y)];
            });
            for (int r : roots) visit(r);
        }
        return order;
    }

    bool emit_node(int id)
    {
        const Node &n = N(id);
        FragmentPlan &fp = sh.frag;
        Src s[3];
        int tmps[3] = {-1, -1, -1};
        for (int k = 0; k < n.nargs; k++) {
            if (!resolve(n.a[k], s[k], tmps[k])) return false;
        }
        /* 源先释放再分配目标：VM 先读完全部源再写目标 */
        for (int k = 0; k < n.nargs; k++) {
            if (tmps[k] >= 0) free_node(tmps[k]);
            release_operand(n.a[k]);
        }
        const uint8_t src[3] = {s[0].reg, n.nargs > 1 ? s[1].reg : ESPGPU_VM_ZERO,
                                n.nargs > 2 ? s[2].reg : ESPGPU_VM_ZERO};
        uint8_t neg = 0;
        for (int k = 0; k < n.nargs; k++) neg |= static_cast<uint8_t>((s[k].neg ? 1u : 0u) << k);

        if (n.kind == NK::Kill) {
            /* index 0：谓词 > 0 丢弃 → KIL(-p)；index 1：操作数 < 0 丢弃 → KIL(x) */
            if (n.index == 0) neg ^= 1;
            const uint8_t swz[3] = {swz_seq(s[0]), ESPGPU_VM_SWZ_IDENTITY, ESPGPU_VM_SWZ_IDENTITY};
            emit_raw(ESPGPU_VM_KIL, ESPGPU_VM_OUT, 0, src, swz, neg);
            return true;
        }

        /* 目标 */
        uint8_t dst;
        uint8_t mask = 0;
        int8_t logical_of_lane[4] = {-1, -1, -1, -1};
        const unsigned used = lanes[static_cast<size_t>(id)] ? lanes[static_cast<size_t>(id)] : 1u;
        if (out_mask[static_cast<size_t>(id)]) {
            dst = ESPGPU_VM_OUT;
            mask = out_mask[static_cast<size_t>(id)];
            for (int l = 0; l < 4; l++) logical_of_lane[l] = out_map[static_cast<size_t>(id)][static_cast<size_t>(l)];
        } else {
            Place pl;
            const bool bcast = n.kind == NK::Op && !componentwise(n.op);
            int need = bcast ? 1 : popcount4(used);
            for (int l = 0; l < 4; l++) pl.lane[l] = (bcast ? l == 0 : (used & (1u << l)) != 0) ? -2 : -1;
            if (!alloc_lanes(id, need, n.kind == NK::Tex ? used : 0u, group_of[static_cast<size_t>(id)], pl)) {
                return false;
            }
            place[static_cast<size_t>(id)] = pl;
            dst = ESPGPU_VM_REG(ESPGPU_VM_BANK_TEMP, pl.reg);
            for (int lg = 0; lg < 4; lg++) {
                if (pl.lane[lg] >= 0) {
                    mask |= static_cast<uint8_t>(1u << pl.lane[lg]);
                    logical_of_lane[pl.lane[lg]] = static_cast<int8_t>(lg);
                }
            }
            if (bcast) {
                /* 广播结果：所有逻辑分量都在同一个物理分量上 */
                const int8_t l0 = pl.lane[0];
                for (int lg = 0; lg < 4; lg++) place[static_cast<size_t>(id)].lane[lg] = l0;
            }
        }
        /* 掩码外的分量随便指向一个有效的逻辑分量 */
        int8_t any = 0;
        for (int l = 0; l < 4; l++) {
            if (logical_of_lane[l] >= 0) {
                any = logical_of_lane[l];
                break;
            }
        }
        for (int l = 0; l < 4; l++) {
            if (logical_of_lane[l] < 0) logical_of_lane[l] = any;
        }

        uint8_t swz[3] = {ESPGPU_VM_SWZ_IDENTITY, ESPGPU_VM_SWZ_IDENTITY, ESPGPU_VM_SWZ_IDENTITY};
        if (n.kind == NK::Tex) {
            swz[0] = swz_seq(s[0]);
            int &u = unit_of_binding[static_cast<size_t>(n.index)];
            if (u < 0) {
                if (fp.unit_sampler.size() >= ESPGPU_MAX_TEXTURE_UNITS) return fail("too many samplers (device has 4 texture units)");
                u = static_cast<int>(fp.unit_sampler.size());
                fp.unit_sampler.push_back(d.samplers[static_cast<size_t>(n.index)]);
            }
            emit_raw(ESPGPU_VM_TEX, dst, mask, src, swz, neg, static_cast<uint8_t>(u));
            return true;
        }
        for (int k = 0; k < n.nargs; k++) swz[k] = componentwise(n.op) ? swz_of(s[k], logical_of_lane) : swz_seq(s[k]);
        emit_raw(n.op, dst, mask, src, swz, neg);
        sh.frag.code.back().flags = n.flags;
        return true;
    }

    bool emit_color()
    {
        if (!d.has_color) return true; /* 没写颜色：o0 保持初值 */
        const Operand &c = d.color;
        std::vector<int> idx;
        for (int i = 0; i < c.n; i++) {
            const int x = c.s[i].node;
            if (out_mask[static_cast<size_t>(x)] & (1u << i)) continue; /* 已直接写进 o0 */
            float k;
            if (isK(c.s[i], &k)) {
                const float v = c.neg ? -k : k;
                if (v == (i == 3 ? 1.0f : 0.0f)) continue; /* 与 o0 初值相同 */
            }
            idx.push_back(i);
        }
        if (idx.empty()) return true;
        /* 剩下的分量都只依赖 uniform：放进一个扩展槽，一条 MOV */
        bool uo_rest = use_pre;
        for (int i : idx) uo_rest = uo_rest && uo[static_cast<size_t>(c.s[i].node)];
        /* 都来自同一个 uniform 寄存器（或都是常量）时直接 MOV，不必再复制进扩展槽 */
        bool one_reg = true;
        for (int i : idx) {
            const Node &n = N(c.s[i].node);
            one_reg = one_reg && n.kind == NK::Uniform && n.index == N(c.s[idx[0]].node).index;
        }
        bool all_k = true;
        for (int i : idx) all_k = all_k && isK(c.s[i]);
        if (one_reg || all_k) uo_rest = false;
        int lane[4] = {0, 1, 2, 3};
        if (uo_rest && idx.size() > 1) {
            std::vector<PreLane> want;
            for (int i : idx) want.push_back(lane_of_scalar(c.s[i]));
            int lo[4];
            const int slot = xplace(want, lo);
            int sw[4] = {0, 0, 0, 0};
            uint8_t mask = 0;
            for (size_t q = 0; q < idx.size(); q++) {
                sw[idx[q]] = lo[q];
                mask |= static_cast<uint8_t>(1u << idx[q]);
            }
            for (int l = 0; l < 4; l++) {
                if (!(mask & (1u << l))) sw[l] = lo[0];
            }
            const uint8_t src[3] = {xreg(slot), ESPGPU_VM_ZERO, ESPGPU_VM_ZERO};
            const uint8_t swz[3] = {ESPGPU_VM_SWZ(sw[0], sw[1], sw[2], sw[3]), ESPGPU_VM_SWZ_IDENTITY,
                                    ESPGPU_VM_SWZ_IDENTITY};
            emit_raw(ESPGPU_VM_MOV, ESPGPU_VM_OUT, mask, src, swz, c.neg ? 1 : 0);
            return true;
        }
        if (use_pre) {
            std::vector<PreLane> want;
            for (int i : idx) {
                if (pre(c.s[i].node)) want.push_back(lane_of_scalar(c.s[i]));
            }
            int lo[4];
            if (!want.empty()) xplace(want, lo);
        }
        assemble(c, idx, ESPGPU_VM_OUT, lane);
        return true;
    }

    bool vm()
    {
        FragmentPlan &fp = sh.frag;
        fp = FragmentPlan{};
        fp.program = true;

        /* 窥孔改写 */
        classify();
        reach();
        rewrite_neg();
        reach();
        if (use_pre) {
            reassoc();
            reach();
            classify();
        }
        fuse_mad();
        sel_cmp();
        kill_direct();
        smooth_flip();
        reach();
        sat_fold();
        reach();
        if (use_slp) {
            slp_all();
            reach();
        }
        classify();

        /* varying → v 寄存器：优先 TEXCOORD0..3、NORMAL，最后才用 COLOR（设备可能夹取颜色） */
        static const int vreg_order[6] = {1, 2, 3, 4, 5, 0};
        static const int slot_of_vreg[6] = {ESPGPU_ATTR_COLOR, ESPGPU_ATTR_TEXCOORD0, ESPGPU_ATTR_TEXCOORD1,
                                            ESPGPU_ATTR_TEXCOORD2, ESPGPU_ATTR_TEXCOORD3, ESPGPU_ATTR_NORMAL};
        vreg_of_input.assign(sh.inputs.size(), -1);
        int nv = 0;
        int maxv = -1;
        for (size_t i = 0; i < sh.inputs.size(); i++) {
            if (!sh.inputs[i].used) continue;
            if (nv >= 6) return fail("too many varyings (device supports 6 vec4)");
            const int vr = vreg_order[nv++];
            vreg_of_input[i] = vr;
            fp.varying_slots.push_back({static_cast<int>(i), slot_of_vreg[vr]});
            maxv = std::max(maxv, vr);
        }
        unit_of_binding.assign(d.samplers.size(), -1);
        place.assign(d.nodes.size(), Place{});
        if (sh.uniform_slot_count > static_cast<int>(ESPGPU_VM_CONSTS)) {
            return fail("too many uniform vectors (" + std::to_string(sh.uniform_slot_count) + " > 32)");
        }

        plan_outputs();
        plan_groups();

        for (int id : live) {
            if (N(id).kind == NK::FragCoord) maxv = std::max(maxv, 6);
        }
        for (int id : schedule()) {
            if (!emit_node(id)) return false;
        }
        if (!emit_color()) return false;

        espgpu_vm_insn_t end{};
        end.op = ESPGPU_VM_END;
        end.dst = ESPGPU_VM_OUT;
        end.swz[0] = end.swz[1] = end.swz[2] = ESPGPU_VM_SWZ_IDENTITY;
        fp.code.push_back(end);

        if (fp.code.size() > ESPGPU_VM_MAX_INSNS) {
            return fail("fragment shader too long: " + std::to_string(fp.code.size()) + " instructions (max " +
                        std::to_string(ESPGPU_VM_MAX_INSNS) + ")");
        }
        fp.varying_count = static_cast<uint8_t>(maxv + 1);
        return finalize_consts();
    }
};

} // namespace

int g_codegen_force_mode = -1;

bool codegen_fragment(Shader &sh)
{
    Dag &d = *sh.dag;
    const std::vector<Node> nodes0 = d.nodes;
    const Operand color0 = d.color;
    const int forced = g_codegen_force_mode;
    if (forced < 0) {
        Gen g(sh);
        g.classify();
        g.reach();
        if (g.try_pattern()) return true;
    }
    /*
     * 几组优化开关各生成一遍，取指令最少的（同长时取常量槽少的）。主机预计算 / 分量打包 /
     * SLP 会多占常量槽或临时寄存器，个别着色器上也可能不划算；编译期多花几遍很便宜。
     */
    struct Best {
        bool ok = false;
        FragmentPlan fp;
        Dag dag;
        std::vector<bool> in_used, u_used;
        size_t len = 0;
        int slots = 0;
    } best;
    std::string first_err;
    for (int mode = 0; mode < 8; mode++) {
        if (forced >= 0 && mode != forced) continue;
        d.nodes = nodes0;
        d.color = color0;
        d.xslots.clear();
        d.pre_order.clear();
        Gen g(sh);
        g.use_pre = (mode & 4) == 0;
        g.use_pack = (mode & 2) == 0;
        g.use_slp = (mode & 1) == 0;
        if (!g.vm()) {
            if (first_err.empty()) first_err = g.err;
            continue;
        }
        const size_t len = sh.frag.code.size();
        if (!best.ok || len < best.len || (len == best.len && sh.frag.const_slots < best.slots)) {
            best.ok = true;
            best.fp = sh.frag;
            best.dag = d;
            best.len = len;
            best.slots = sh.frag.const_slots;
            best.in_used.clear();
            best.u_used.clear();
            for (const auto &in : sh.inputs) best.in_used.push_back(in.used);
            for (const auto &u : sh.uniforms) best.u_used.push_back(u.used);
        }
    }
    if (best.ok) {
        sh.frag = best.fp;
        d = best.dag;
        for (size_t i = 0; i < sh.inputs.size(); i++) sh.inputs[i].used = best.in_used[i];
        for (size_t i = 0; i < sh.uniforms.size(); i++) sh.uniforms[i].used = best.u_used[i];
        return true;
    }
    d.nodes = nodes0;
    d.color = color0;
    sh.frag = FragmentPlan{};
    sh.log += "1:1: error: " + first_err + "\n";
    return false;
}

} // namespace espgpu::glsl
