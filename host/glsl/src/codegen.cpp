/*
 * codegen.cpp — 片元着色器：模式识别 → 内置着色模式；否则生成 VM 字节码
 *
 * 寄存器分配是线性扫描：节点按 id 顺序（即拓扑序）发射，每个结果占一个
 * vec4 临时寄存器，最后一次使用后释放。混合来源的操作数（比如 vec4(a.x, b.y, 1, 0)）
 * 用几条带写掩码的 MOV 先拼进一个临时寄存器。常量打包进 uniform 之后的 c 槽，
 * 全 0 / 全 1 走特殊寄存器不占槽。
 */

#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace espgpu::glsl {

namespace {

struct Gen {
    Shader &sh;
    Dag &d;
    std::string err;

    std::vector<int> live;      /* 需要发射的节点 */
    std::vector<int> uses;      /* 每个节点被引用的次数（剩余） */
    std::vector<int> temp_of;   /* 节点 → 临时寄存器 */
    bool temp_busy[ESPGPU_VM_TEMPS] = {};
    std::vector<int> vreg_of_input;
    std::vector<int> unit_of_binding;
    struct CSlot {
        float v[4];
        int n = 0;
    };
    std::vector<CSlot> cslots;

    Gen(Shader &s) : sh(s), d(*s.dag) {}

    bool fail(const std::string &m)
    {
        if (err.empty()) err = m;
        return false;
    }

    const Node &N(int id) const { return d.nodes[static_cast<size_t>(id)]; }

    /* ── 可达性 ── */

    void mark(std::vector<char> &seen, int id)
    {
        if (id < 0 || seen[static_cast<size_t>(id)]) return;
        seen[static_cast<size_t>(id)] = 1;
        const Node &n = N(id);
        for (int k = 0; k < n.nargs; k++) {
            for (int i = 0; i < n.a[k].n; i++) mark(seen, n.a[k].s[i].node);
        }
    }

    void count_uses(const Operand &o)
    {
        std::set<int> distinct;
        for (int i = 0; i < o.n; i++) distinct.insert(o.s[i].node);
        for (int id : distinct) uses[static_cast<size_t>(id)]++;
    }

    void reach()
    {
        std::vector<char> seen(d.nodes.size(), 0);
        if (d.has_color) {
            for (int i = 0; i < d.color.n; i++) mark(seen, d.color.s[i].node);
        }
        for (int k : d.kills) mark(seen, k);
        uses.assign(d.nodes.size(), 0);
        for (size_t id = 0; id < d.nodes.size(); id++) {
            if (!seen[id]) continue;
            const Node &n = d.nodes[id];
            live.push_back(static_cast<int>(id));
            for (int k = 0; k < n.nargs; k++) count_uses(n.a[k]);
            if (n.kind == NK::Varying && n.index >= 0) sh.inputs[static_cast<size_t>(n.index)].used = true;
            if (n.kind == NK::Uniform) mark_uniform_slot(n.index);
            if (n.kind == NK::Tex && n.index >= 0) {
                sh.uniforms[static_cast<size_t>(d.samplers[static_cast<size_t>(n.index)].first)].used = true;
            }
        }
        if (d.has_color) count_uses(d.color);
    }

    void mark_uniform_slot(int slot)
    {
        for (auto &u : sh.uniforms) {
            if (!u.is_sampler && slot >= u.local_slot && slot < u.local_slot + u.array_size * u.columns) u.used = true;
        }
    }

    /** MUL 结果只被一个 ADD 用到时合并成 MAD（plasma 一类的着色器能省 20% 指令） */
    void fuse_mad()
    {
        bool changed = false;
        for (int id : live) {
            Node &n = d.nodes[static_cast<size_t>(id)];
            if (n.kind != NK::Op || n.op != ESPGPU_VM_ADD) continue;
            for (int k = 0; k < 2; k++) {
                const Operand &o = n.a[k];
                const int m = o.s[0].node;
                bool ok = true;
                for (int i = 0; i < o.n; i++) ok = ok && o.s[i].node == m && o.s[i].comp == i;
                const Node &mn = N(m);
                if (!ok || mn.kind != NK::Op || mn.op != ESPGPU_VM_MUL || mn.width != o.n ||
                    uses[static_cast<size_t>(m)] != 1) {
                    continue;
                }
                const Operand other = n.a[1 - k];
                n.op = ESPGPU_VM_MAD;
                n.nargs = 3;
                n.a[0] = mn.a[0];
                n.a[1] = mn.a[1];
                n.a[2] = other;
                uses[static_cast<size_t>(m)] = 0;
                changed = true;
                break;
            }
        }
        if (changed) {
            live.clear();
            reach();
        }
    }

    /* ── 模式识别 ── */

    bool whole(const Operand &o, int &node) const
    {
        if (o.n != 4) return false;
        for (int i = 0; i < 4; i++) {
            if (o.s[i].node != o.s[0].node || o.s[i].comp != i) return false;
        }
        node = o.s[0].node;
        return true;
    }

    struct Factor {
        enum { Tex, Color, Uniform } kind;
        int binding = -1; /* Tex */
        int input = -1;   /* Tex 的坐标 varying / Color 的 varying */
        int slot = -1;    /* Uniform */
    };

    bool factors(int id, std::vector<Factor> &out) const
    {
        const Node &n = N(id);
        if (n.kind == NK::Varying && n.index >= 0 && n.width == 4) {
            out.push_back({Factor::Color, -1, n.index, -1});
            return true;
        }
        if (n.kind == NK::Uniform) {
            out.push_back({Factor::Uniform, -1, -1, n.index});
            return true;
        }
        if (n.kind == NK::Tex) {
            const Operand &c = n.a[0];
            if (c.n != 2 || c.s[0].node != c.s[1].node || c.s[0].comp != 0 || c.s[1].comp != 1) return false;
            const Node &v = N(c.s[0].node);
            if (v.kind != NK::Varying || v.index < 0) return false;
            out.push_back({Factor::Tex, n.index, v.index, -1});
            return true;
        }
        if (n.kind == NK::Op && n.op == ESPGPU_VM_MUL && n.width == 4) {
            int a, b;
            if (!whole(n.a[0], a) || !whole(n.a[1], b)) return false;
            return factors(a, out) && factors(b, out);
        }
        return false;
    }

    bool try_pattern()
    {
        FragmentPlan &fp = sh.frag;
        if (!d.has_color) return false;
        int out;
        if (!whole(d.color, out)) return false;
        std::vector<Factor> f;
        if (!factors(out, f)) return false;

        /* alpha 测试：唯一的 KIL，条件是 out.a 与常量比较 */
        bool at = false;
        uint8_t func = ESPGPU_CMP_ALWAYS, ref = 0;
        if (d.kills.size() > 1) return false;
        if (d.kills.size() == 1) {
            const Node &k = N(d.kills[0]);
            const Scalar p = k.a[0].s[0];
            const Node &c = N(p.node);
            if (c.kind != NK::Op || c.width < 1) return false;
            const Scalar x = c.a[0].s[p.comp], y = c.a[1].s[p.comp];
            const Scalar alpha{out, 3};
            float kv;
            if (c.op == ESPGPU_VM_SLT && x == alpha && N(y.node).kind == NK::Const) {
                kv = N(y.node).c; /* a < k 丢弃 → 保留 a >= k */
                func = ESPGPU_CMP_GEQUAL;
            } else if (c.op == ESPGPU_VM_SGE && y == alpha && N(x.node).kind == NK::Const) {
                kv = N(x.node).c; /* k >= a 丢弃 → 保留 a > k */
                func = ESPGPU_CMP_GREATER;
            } else {
                return false;
            }
            const float r = std::round(std::min(std::max(kv, 0.0f), 1.0f) * 255.0f);
            ref = static_cast<uint8_t>(r);
            at = true;
        }

        std::vector<Factor> tex, col;
        for (const auto &x : f) (x.kind == Factor::Tex ? tex : col).push_back(x);
        if (tex.size() > 2 || col.size() > 1) return false;

        fp = FragmentPlan{};
        fp.program = false;
        fp.alpha_test = at;
        fp.alpha_func = func;
        fp.alpha_ref = ref;
        if (!col.empty()) {
            if (col[0].kind == Factor::Color) {
                fp.color_input = col[0].input;
                fp.varying_slots.push_back({col[0].input, ESPGPU_ATTR_COLOR});
            } else {
                fp.color_uniform_slot = col[0].slot;
            }
        }
        for (size_t i = 0; i < tex.size(); i++) {
            fp.varying_slots.push_back({tex[i].input, static_cast<int>(ESPGPU_ATTR_TEXCOORD0 + i)});
            fp.unit_sampler.push_back(d.samplers[static_cast<size_t>(tex[i].binding)]);
        }
        if (tex.empty()) {
            if (col.empty()) return false;
            fp.shade_mode = ESPGPU_SHADE_FLAT;
        } else if (tex.size() == 1) {
            fp.shade_mode = col.empty() ? ESPGPU_SHADE_TEX : ESPGPU_SHADE_TEX_MOD_COLOR;
        } else {
            fp.shade_mode = ESPGPU_SHADE_TEX_MOD_TEX;
            if (col.empty()) fp.color_white = true;
        }
        return true;
    }

    /* ── VM 代码生成 ── */

    struct Src {
        uint8_t reg = ESPGPU_VM_ZERO;
        uint8_t swz = ESPGPU_VM_SWZ_IDENTITY;
    };

    int alloc_temp()
    {
        for (int i = 0; i < static_cast<int>(ESPGPU_VM_TEMPS); i++) {
            if (!temp_busy[i]) {
                temp_busy[i] = true;
                return i;
            }
        }
        fail("fragment shader needs more than 16 live temporaries");
        return 0;
    }

    void release(int node)
    {
        int &u = uses[static_cast<size_t>(node)];
        if (--u == 0 && temp_of[static_cast<size_t>(node)] >= 0) {
            temp_busy[temp_of[static_cast<size_t>(node)]] = false;
        }
    }

    void release_operand(const Operand &o)
    {
        std::set<int> distinct;
        for (int i = 0; i < o.n; i++) distinct.insert(o.s[i].node);
        for (int id : distinct) release(id);
    }

    /** 标量所在寄存器；常量返回 false */
    bool where(const Scalar &s, uint8_t &reg, int &comp)
    {
        const Node &n = N(s.node);
        comp = s.comp;
        switch (n.kind) {
        case NK::Op:
        case NK::Tex:
            reg = ESPGPU_VM_REG(ESPGPU_VM_BANK_TEMP, temp_of[static_cast<size_t>(s.node)]);
            return true;
        case NK::Varying:
            reg = ESPGPU_VM_REG(ESPGPU_VM_BANK_VARYING, vreg_of_input[static_cast<size_t>(n.index)]);
            return true;
        case NK::FragCoord:
            reg = ESPGPU_VM_REG(ESPGPU_VM_BANK_VARYING, 6);
            return true;
        case NK::Uniform:
            reg = ESPGPU_VM_REG(ESPGPU_VM_BANK_CONST, n.index);
            return true;
        default: return false;
        }
    }

    /** 把一组常量放进同一个 c 槽，返回槽号与每个值的分量 */
    int place_consts(const std::vector<float> &vals, int comp_of[4])
    {
        std::vector<float> uniq;
        for (float v : vals) {
            bool dup = false;
            for (float u : uniq) dup = dup || std::memcmp(&u, &v, 4) == 0;
            if (!dup) uniq.push_back(v);
        }
        auto find = [](const CSlot &cs, float v) {
            for (int i = 0; i < cs.n; i++) {
                if (std::memcmp(&cs.v[i], &v, 4) == 0) return i;
            }
            return -1;
        };
        int slot = -1;
        for (size_t s = 0; s < cslots.size() && slot < 0; s++) {
            int missing = 0;
            for (float v : uniq) missing += find(cslots[s], v) < 0;
            if (cslots[s].n + missing <= 4) slot = static_cast<int>(s);
        }
        if (slot < 0) {
            cslots.push_back({});
            slot = static_cast<int>(cslots.size()) - 1;
        }
        CSlot &cs = cslots[static_cast<size_t>(slot)];
        for (float v : uniq) {
            if (find(cs, v) < 0) cs.v[cs.n++] = v;
        }
        for (size_t i = 0; i < vals.size(); i++) comp_of[i] = find(cs, vals[i]);
        return slot;
    }

    uint8_t const_reg(int slot) const
    {
        return ESPGPU_VM_REG(ESPGPU_VM_BANK_CONST, sh.uniform_slot_count + slot);
    }

    static uint8_t make_swz(const int comps[4], int n)
    {
        int c[4];
        for (int i = 0; i < 4; i++) c[i] = comps[std::min(i, n - 1)];
        return ESPGPU_VM_SWZ(c[0], c[1], c[2], c[3]);
    }

    void emit(uint8_t op, uint8_t dst, uint8_t mask, const Src *s, int ns, uint8_t neg = 0, uint8_t unit = 0)
    {
        espgpu_vm_insn_t in{};
        in.op = op;
        in.dst = dst;
        in.mask = mask;
        in.flags = 0;
        for (int k = 0; k < 3; k++) {
            in.src[k] = k < ns ? s[k].reg : ESPGPU_VM_ZERO;
            in.swz[k] = k < ns ? s[k].swz : ESPGPU_VM_SWZ_IDENTITY;
        }
        in.neg = neg;
        in.tex_unit = unit;
        sh.frag.code.push_back(in);
    }

    /** 解析操作数；需要拼装时 tmp 返回临时寄存器（用完调用方释放），否则 -1。
     *  into_out：需要拼装时直接拼进 o0（最终颜色用） */
    Src resolve(const Operand &o, int &tmp, bool into_out = false)
    {
        tmp = -1;
        Src r;
        const int n = o.n;
        uint8_t regs[4];
        int comps[4];
        bool is_reg[4];
        bool all_const = true;
        for (int i = 0; i < n; i++) {
            is_reg[i] = where(o.s[i], regs[i], comps[i]);
            all_const = all_const && !is_reg[i];
        }
        if (all_const) {
            bool zero = true, one = true;
            std::vector<float> vals;
            for (int i = 0; i < n; i++) {
                const float v = N(o.s[i].node).c;
                zero = zero && v == 0.0f;
                one = one && v == 1.0f;
                vals.push_back(v);
            }
            if (zero) return {ESPGPU_VM_ZERO, ESPGPU_VM_SWZ_IDENTITY};
            if (one) return {ESPGPU_VM_ONE, ESPGPU_VM_SWZ_IDENTITY};
            int cc[4];
            const int slot = place_consts(vals, cc);
            return {const_reg(slot), make_swz(cc, n)};
        }
        bool same = true;
        for (int i = 0; i < n; i++) same = same && is_reg[i] && regs[i] == regs[0];
        if (same) return {regs[0], make_swz(comps, n)};

        /* 拼装：按来源寄存器分组 MOV 进临时寄存器 */
        uint8_t dst;
        if (into_out) {
            dst = ESPGPU_VM_OUT;
        } else {
            tmp = alloc_temp();
            dst = ESPGPU_VM_REG(ESPGPU_VM_BANK_TEMP, tmp);
        }
        std::vector<bool> done(static_cast<size_t>(n), false);
        for (int i = 0; i < n; i++) {
            if (done[static_cast<size_t>(i)]) continue;
            int sc[4] = {0, 0, 0, 0};
            uint8_t mask = 0;
            Src s;
            if (is_reg[i]) {
                for (int j = i; j < n; j++) {
                    if (!done[static_cast<size_t>(j)] && is_reg[j] && regs[j] == regs[i]) {
                        mask |= static_cast<uint8_t>(1u << j);
                        sc[j] = comps[j];
                        done[static_cast<size_t>(j)] = true;
                    }
                }
                s.reg = regs[i];
            } else {
                std::vector<float> vals;
                std::vector<int> idx;
                for (int j = i; j < n; j++) {
                    if (!done[static_cast<size_t>(j)] && !is_reg[j]) {
                        vals.push_back(N(o.s[j].node).c);
                        idx.push_back(j);
                        done[static_cast<size_t>(j)] = true;
                        mask |= static_cast<uint8_t>(1u << j);
                    }
                }
                int cc[4];
                const int slot = place_consts(vals, cc);
                for (size_t k = 0; k < idx.size(); k++) sc[idx[k]] = cc[k];
                s.reg = const_reg(slot);
            }
            s.swz = ESPGPU_VM_SWZ(sc[0], sc[1], sc[2], sc[3]);
            emit(ESPGPU_VM_MOV, dst, mask, &s, 1);
        }
        int id[4] = {0, 1, 2, 3};
        r.reg = dst;
        r.swz = make_swz(id, n);
        return r;
    }

    bool vm()
    {
        FragmentPlan &fp = sh.frag;
        fp = FragmentPlan{};
        fp.program = true;

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

        temp_of.assign(d.nodes.size(), -1);
        if (sh.uniform_slot_count > static_cast<int>(ESPGPU_VM_CONSTS)) {
            return fail("too many uniform vectors (" + std::to_string(sh.uniform_slot_count) + " > 32)");
        }

        std::set<int> kills(d.kills.begin(), d.kills.end());
        for (int id : live) {
            const Node &n = N(id);
            if (n.kind == NK::FragCoord) maxv = std::max(maxv, 6);
            if (n.kind != NK::Op && n.kind != NK::Tex && n.kind != NK::Kill) continue;

            Src s[3];
            int tmps[3] = {-1, -1, -1};
            for (int k = 0; k < n.nargs; k++) s[k] = resolve(n.a[k], tmps[k]);
            if (!err.empty()) return false;

            if (n.kind == NK::Kill) {
                emit(ESPGPU_VM_KIL, ESPGPU_VM_OUT, 0, s, 1, 1);
            } else {
                /* 先分配目标再释放参数，避免同一条指令里读写冲突 */
                const int t = alloc_temp();
                temp_of[static_cast<size_t>(id)] = t;
                const uint8_t mask = static_cast<uint8_t>((1u << n.width) - 1u);
                if (n.kind == NK::Tex) {
                    int &u = unit_of_binding[static_cast<size_t>(n.index)];
                    if (u < 0) {
                        if (fp.unit_sampler.size() >= ESPGPU_MAX_TEXTURE_UNITS) return fail("too many samplers (device has 4 texture units)");
                        u = static_cast<int>(fp.unit_sampler.size());
                        fp.unit_sampler.push_back(d.samplers[static_cast<size_t>(n.index)]);
                    }
                    emit(ESPGPU_VM_TEX, ESPGPU_VM_REG(ESPGPU_VM_BANK_TEMP, t), 0xF, s, 1, 0, static_cast<uint8_t>(u));
                } else {
                    emit(n.op, ESPGPU_VM_REG(ESPGPU_VM_BANK_TEMP, t), mask, s, n.nargs);
                }
                if (uses[static_cast<size_t>(id)] == 0) temp_busy[t] = false;
            }
            for (int k = 0; k < n.nargs; k++) {
                if (tmps[k] >= 0) temp_busy[tmps[k]] = false;
                release_operand(n.a[k]);
            }
            if (!err.empty()) return false;
        }

        /* 输出 */
        if (d.has_color) {
            int tmp;
            const size_t before = fp.code.size();
            Src s = resolve(d.color, tmp, true);
            if (s.reg != ESPGPU_VM_OUT || fp.code.size() == before) {
                emit(ESPGPU_VM_MOV, ESPGPU_VM_OUT, 0xF, &s, 1);
            }
        } else {
            Src z{ESPGPU_VM_ZERO, ESPGPU_VM_SWZ_IDENTITY};
            emit(ESPGPU_VM_MOV, ESPGPU_VM_OUT, 0xF, &z, 1);
        }
        espgpu_vm_insn_t end{};
        end.op = ESPGPU_VM_END;
        end.dst = ESPGPU_VM_OUT;
        end.swz[0] = end.swz[1] = end.swz[2] = ESPGPU_VM_SWZ_IDENTITY;
        fp.code.push_back(end);

        if (fp.code.size() > ESPGPU_VM_MAX_INSNS) {
            return fail("fragment shader too long: " + std::to_string(fp.code.size()) + " instructions (max " +
                        std::to_string(ESPGPU_VM_MAX_INSNS) + ")");
        }
        fp.uniform_slots = sh.uniform_slot_count;
        for (size_t i = 0; i < cslots.size(); i++) {
            FragmentPlan::ConstSlot cs;
            cs.slot = sh.uniform_slot_count + static_cast<int>(i);
            cs.v = {0, 0, 0, 0};
            for (int k = 0; k < cslots[i].n; k++) cs.v[static_cast<size_t>(k)] = cslots[i].v[k];
            fp.consts.push_back(cs);
        }
        if (sh.uniform_slot_count + static_cast<int>(cslots.size()) > static_cast<int>(ESPGPU_VM_CONSTS)) {
            return fail("uniforms + constants exceed 32 vec4 registers");
        }
        fp.varying_count = static_cast<uint8_t>(maxv + 1);
        return true;
    }
};

} // namespace

bool codegen_fragment(Shader &sh)
{
    Gen g(sh);
    g.reach();
    if (g.try_pattern()) return true;
    g.fuse_mad();
    if (!g.vm()) {
        sh.log += "1:1: error: " + g.err + "\n";
        return false;
    }
    return true;
}

} // namespace espgpu::glsl
