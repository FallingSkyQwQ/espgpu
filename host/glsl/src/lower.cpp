/*
 * lower.cpp — 符号执行：AST → 无分支 DAG
 *
 * 每个变量保存为一串"标量引用"（DAG 节点 + 分量）。执行语句时：
 *   - 表达式直接建 DAG 节点，参数全是常量就当场折叠（循环计数器因此可以折叠）
 *   - if/else 不产生分支：两侧都执行，赋值按当前谓词写成 SEL(谓词, 新值, 旧值)
 *   - return / break / continue 把当前谓词累加进退出集合，之后的语句谓词扣掉它
 *   - 循环按常量次数展开，直到谓词折叠成常量 0
 *   - 函数调用直接内联
 *   - discard 生成 KIL（条件 = 当前谓词）
 *
 * 类型检查与执行在同一遍完成；没被调用的函数会在谓词恒 0 的"空跑"里检查一遍。
 */

#include "internal.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <set>

namespace espgpu::glsl {

namespace {

struct CompileError {
    SrcLoc loc;
    std::string msg;
};

[[noreturn]] void error(SrcLoc l, const std::string &m)
{
    throw CompileError{l, m};
}

enum class BT { Void, Float, Int, UInt, Bool, Sampler, Struct };

struct Ty {
    BT bt = BT::Float;
    int vec = 1;  /**< 行数 / 向量分量数 */
    int cols = 1; /**< 矩阵列数 */
    int array = 0;
    int st = -1;
    bool operator==(const Ty &o) const
    {
        return bt == o.bt && vec == o.vec && cols == o.cols && array == o.array && st == o.st;
    }
    bool operator!=(const Ty &o) const { return !(*this == o); }
    bool is_array() const { return array > 0; }
    bool is_scalar() const
    {
        return array == 0 && bt != BT::Struct && bt != BT::Sampler && bt != BT::Void && vec == 1 &&
               cols == 1;
    }
    bool is_vector() const { return array == 0 && cols == 1 && vec > 1 && bt != BT::Struct; }
    bool is_matrix() const { return array == 0 && cols > 1; }
    bool numeric() const { return bt == BT::Float || bt == BT::Int || bt == BT::UInt; }
    bool is_int() const { return bt == BT::Int || bt == BT::UInt; }
    Ty elem() const
    {
        Ty e = *this;
        e.array = 0;
        return e;
    }
    Ty column() const
    {
        Ty e = *this;
        e.array = 0;
        e.cols = 1;
        return e;
    }
    Ty with_vec(int n) const
    {
        Ty e = *this;
        e.vec = n;
        e.cols = 1;
        e.array = 0;
        return e;
    }
};

Ty T(BT b, int v = 1, int c = 1)
{
    Ty t;
    t.bt = b;
    t.vec = v;
    t.cols = c;
    return t;
}

struct SDef {
    std::string name;
    std::vector<std::pair<std::string, Ty>> members;
};

struct Val {
    Ty ty;
    std::vector<Scalar> s;
    std::vector<int> samp; /**< 采样器绑定，每个元素一个 */
};

struct Var {
    Ty ty;
    std::vector<Scalar> s;
    std::vector<int> samp;
    bool readonly = false;
    bool is_const = false;
    int loop_id = -1;  /**< 在 for 的初始化语句里声明：属于哪个循环 */
    std::string what; /**< 只读原因，用于报错 */
};
using VarP = std::shared_ptr<Var>;

struct Alt {
    Scalar cond;
    std::vector<int> pos;
};

struct LVal {
    VarP var;
    Ty ty;
    std::vector<Alt> alts;
    std::vector<int> samp_idx; /**< 采样器类型时：samp 下标 */
    bool swizzle_dup = false;
};

constexpr int kMaxUnroll = 256;

class Lower {
public:
    Lower(const TranslationUnit &tu, Stage st, int version, Shader &sh)
        : tu_(tu), stage_(st), version_(version), sh_(sh), dag_(*sh.dag)
    {
    }

    bool run();

private:
    const TranslationUnit &tu_;
    Stage stage_;
    int version_;
    Shader &sh_;
    Dag &dag_;

    std::vector<SDef> structs_;
    std::map<std::string, int> struct_by_name_;
    std::map<const StructDecl *, int> struct_by_decl_;
    std::map<std::string, std::vector<const Function *>> funcs_;

    std::vector<std::map<std::string, VarP>> scopes_;
    std::map<std::vector<int>, int> cse_;
    std::map<uint32_t, int> konst_;

    Scalar pred_;
    Scalar ret_acc_, brk_acc_, cont_acc_;
    bool in_loop_ = false;
    int cur_loop_id_ = -1;
    int next_loop_id_ = 0;
    Scalar cur_loop_lp_;
    struct Frame {
        Ty ret;
        Val retval;
        bool has_ret = false;
    };
    std::vector<Frame> frames_;
    std::vector<const Function *> call_stack_;

    /* 输出变量 */
    VarP out_color_;
    int out_color_comps_ = 4;
    VarP gl_position_;
    std::vector<VarP> var_outs_;

    /* ── DAG 构建 ── */

    int add_node(const Node &n)
    {
        std::vector<int> key = {static_cast<int>(n.kind), n.op, n.width, n.nargs, n.index};
        uint32_t cb;
        std::memcpy(&cb, &n.c, 4);
        key.push_back(static_cast<int>(cb));
        for (int i = 0; i < n.nargs; i++) {
            key.push_back(n.a[i].n);
            for (int k = 0; k < n.a[i].n; k++) {
                key.push_back(n.a[i].s[k].node);
                key.push_back(n.a[i].s[k].comp);
            }
        }
        if (n.kind != NK::Kill) {
            auto it = cse_.find(key);
            if (it != cse_.end()) return it->second;
        }
        dag_.nodes.push_back(n);
        const int id = static_cast<int>(dag_.nodes.size()) - 1;
        if (n.kind != NK::Kill) cse_[key] = id;
        return id;
    }

    Scalar K(float v)
    {
        if (v == 0.0f) v = 0.0f; /* -0 归一 */
        uint32_t b;
        std::memcpy(&b, &v, 4);
        auto it = konst_.find(b);
        if (it != konst_.end()) return {it->second, 0};
        Node n;
        n.kind = NK::Const;
        n.c = v;
        n.width = 1;
        const int id = add_node(n);
        konst_[b] = id;
        return {id, 0};
    }

    bool isK(Scalar s, float *v = nullptr) const
    {
        const Node &n = dag_.nodes[static_cast<size_t>(s.node)];
        if (n.kind != NK::Const) return false;
        if (v) *v = n.c;
        return true;
    }
    bool isK(Scalar s, float want) const
    {
        float v;
        return isK(s, &v) && v == want;
    }

    std::vector<Scalar> outs(int node, int w)
    {
        std::vector<Scalar> r;
        for (int i = 0; i < w; i++) r.push_back({node, static_cast<uint8_t>(i)});
        return r;
    }

    static Operand operand(const std::vector<Scalar> &v, size_t off, size_t n)
    {
        Operand o;
        o.n = static_cast<uint8_t>(n);
        for (size_t i = 0; i < n; i++) o.s[i] = v[off + i];
        return o;
    }

    /** 分量运算。nargs 个长度为 n 的标量串 → n 个结果标量 */
    std::vector<Scalar> cw(uint8_t op, const std::vector<Scalar> &A, const std::vector<Scalar> &B = {},
                           const std::vector<Scalar> &C = {})
    {
        const size_t n = A.size();
        const int nargs = C.empty() ? (B.empty() ? 1 : 2) : 3;
        std::vector<Scalar> res(n);
        std::vector<size_t> pending;
        for (size_t i = 0; i < n; i++) {
            const Scalar a = A[i];
            const Scalar b = nargs > 1 ? B[i] : Scalar{};
            const Scalar c = nargs > 2 ? C[i] : Scalar{};
            float va = 0, vb = 0, vc = 0;
            const bool ka = isK(a, &va);
            const bool kb = nargs > 1 && isK(b, &vb);
            const bool kc = nargs > 2 && isK(c, &vc);
            if (ka && (nargs < 2 || kb) && (nargs < 3 || kc)) {
                float fa[4] = {va, va, va, va}, fb[4] = {vb, vb, vb, vb}, fc[4] = {vc, vc, vc, vc},
                      d[4] = {0, 0, 0, 0};
                eval_op(op, fa, fb, fc, d);
                res[i] = K(d[0]);
                continue;
            }
            /* 代数化简 */
            bool done = true;
            switch (op) {
            case ESPGPU_VM_MOV: res[i] = a; break;
            case ESPGPU_VM_MUL:
                if (ka && va == 1) res[i] = b;
                else if (kb && vb == 1) res[i] = a;
                else if ((ka && va == 0) || (kb && vb == 0)) res[i] = K(0);
                else done = false;
                break;
            case ESPGPU_VM_ADD:
                if (ka && va == 0) res[i] = b;
                else if (kb && vb == 0) res[i] = a;
                else done = false;
                break;
            case ESPGPU_VM_SUB:
                if (kb && vb == 0) res[i] = a;
                else if (a == b) res[i] = K(0);
                else done = false;
                break;
            case ESPGPU_VM_DIV:
                if (kb && vb == 1) res[i] = a;
                else done = false;
                break;
            case ESPGPU_VM_MAD:
                if (kc && vc == 0) {
                    res[i] = cw(ESPGPU_VM_MUL, {a}, {b})[0];
                } else if ((ka && va == 0) || (kb && vb == 0)) {
                    res[i] = c;
                } else if (ka && va == 1) {
                    res[i] = cw(ESPGPU_VM_ADD, {b}, {c})[0];
                } else if (kb && vb == 1) {
                    res[i] = cw(ESPGPU_VM_ADD, {a}, {c})[0];
                } else {
                    done = false;
                }
                break;
            case ESPGPU_VM_SEL:
                if (ka) res[i] = (va != 0) ? b : c;
                else if (b == c) res[i] = b;
                else done = false;
                break;
            case ESPGPU_VM_CMP:
                if (ka) res[i] = (va < 0) ? b : c;
                else if (b == c) res[i] = b;
                else done = false;
                break;
            case ESPGPU_VM_LRP:
                if (ka && va == 0) res[i] = b;
                else if (ka && va == 1) res[i] = c;
                else if (b == c) res[i] = b;
                else done = false;
                break;
            case ESPGPU_VM_MAX:
            case ESPGPU_VM_MIN:
                if (a == b) res[i] = a;
                else done = false;
                break;
            default: done = false; break;
            }
            if (!done) pending.push_back(i);
        }
        /* 剩下的分量按 4 个一组打包成节点 */
        for (size_t g = 0; g < pending.size(); g += 4) {
            const size_t w = std::min<size_t>(4, pending.size() - g);
            Node nd;
            nd.kind = NK::Op;
            nd.op = op;
            nd.width = static_cast<uint8_t>(w);
            nd.nargs = static_cast<uint8_t>(nargs);
            for (int k = 0; k < nargs; k++) {
                const std::vector<Scalar> &src = k == 0 ? A : (k == 1 ? B : C);
                nd.a[k].n = static_cast<uint8_t>(w);
                for (size_t j = 0; j < w; j++) nd.a[k].s[j] = src[pending[g + j]];
            }
            const int id = add_node(nd);
            for (size_t j = 0; j < w; j++) res[pending[g + j]] = {id, static_cast<uint8_t>(j)};
        }
        return res;
    }

    Scalar cw1(uint8_t op, Scalar a, Scalar b = {}, Scalar c = {})
    {
        if (c.node >= 0) return cw(op, {a}, {b}, {c})[0];
        if (b.node >= 0) return cw(op, {a}, {b})[0];
        return cw(op, {a})[0];
    }

    std::vector<Scalar> splat(Scalar s, size_t n) { return std::vector<Scalar>(n, s); }

    Scalar dot(const std::vector<Scalar> &A, const std::vector<Scalar> &B)
    {
        const size_t n = A.size();
        if (n == 1) return cw1(ESPGPU_VM_MUL, A[0], B[0]);
        bool allk = true;
        for (size_t i = 0; i < n; i++) allk = allk && isK(A[i]) && isK(B[i]);
        if (allk) {
            float s = 0;
            for (size_t i = 0; i < n; i++) {
                float a, b;
                isK(A[i], &a);
                isK(B[i], &b);
                s += a * b;
            }
            return K(s);
        }
        Node nd;
        nd.kind = NK::Op;
        nd.op = n == 2 ? ESPGPU_VM_DP2 : (n == 3 ? ESPGPU_VM_DP3 : ESPGPU_VM_DP4);
        nd.width = 1;
        nd.nargs = 2;
        nd.a[0] = operand(A, 0, n);
        nd.a[1] = operand(B, 0, n);
        return {add_node(nd), 0};
    }

    Scalar unary_bcast(uint8_t op, Scalar a)
    {
        float v;
        if (isK(a, &v)) {
            float fa[4] = {v, v, v, v}, z[4] = {0, 0, 0, 0}, d[4];
            eval_op(op, fa, z, z, d);
            return K(d[0]);
        }
        Node nd;
        nd.kind = NK::Op;
        nd.op = op;
        nd.width = 1;
        nd.nargs = 1;
        nd.a[0] = operand({a}, 0, 1);
        return {add_node(nd), 0};
    }

    Scalar pand(Scalar a, Scalar b) { return cw1(ESPGPU_VM_MUL, a, b); }
    Scalar pnot(Scalar a) { return cw1(ESPGPU_VM_SUB, K(1), a); }
    Scalar psub(Scalar a, Scalar b) { return cw1(ESPGPU_VM_SUB, a, b); }
    Scalar padd(Scalar a, Scalar b) { return cw1(ESPGPU_VM_ADD, a, b); }

    /* ── 类型 ── */

    int count(const Ty &t) const
    {
        int base;
        if (t.bt == BT::Struct) {
            base = 0;
            for (const auto &m : structs_[static_cast<size_t>(t.st)].members) base += count(m.second);
        } else if (t.bt == BT::Sampler || t.bt == BT::Void) {
            base = 0;
        } else {
            base = t.vec * t.cols;
        }
        return t.array ? base * t.array : base;
    }

    int samp_count(const Ty &t) const
    {
        if (t.bt != BT::Sampler) return 0;
        return t.array ? t.array : 1;
    }

    std::string tname(const Ty &t) const
    {
        std::string s;
        const char *pre = t.bt == BT::Int ? "i" : t.bt == BT::UInt ? "u" : t.bt == BT::Bool ? "b" : "";
        switch (t.bt) {
        case BT::Void: s = "void"; break;
        case BT::Sampler: s = "sampler2D"; break;
        case BT::Struct: s = structs_[static_cast<size_t>(t.st)].name; break;
        default:
            if (t.cols > 1) {
                s = "mat" + std::to_string(t.cols);
                if (t.vec != t.cols) s += "x" + std::to_string(t.vec);
            } else if (t.vec > 1) {
                s = std::string(pre) + "vec" + std::to_string(t.vec);
            } else {
                s = t.bt == BT::Int ? "int" : t.bt == BT::UInt ? "uint" : t.bt == BT::Bool ? "bool" : "float";
            }
        }
        if (t.array) s += "[" + std::to_string(t.array) + "]";
        return s;
    }

    Ty resolve(const TypeSpec &ts)
    {
        Ty t;
        const std::string &n = ts.name;
        if (ts.inline_struct) {
            t.bt = BT::Struct;
            t.st = define_struct(*ts.inline_struct);
        } else if (n == "void") t = T(BT::Void);
        else if (n == "float") t = T(BT::Float);
        else if (n == "int") t = T(BT::Int);
        else if (n == "uint") t = T(BT::UInt);
        else if (n == "bool") t = T(BT::Bool);
        else if (n.size() == 4 && n.rfind("vec", 0) == 0) t = T(BT::Float, n[3] - '0');
        else if (n.size() == 5 && n.rfind("ivec", 0) == 0) t = T(BT::Int, n[4] - '0');
        else if (n.size() == 5 && n.rfind("uvec", 0) == 0) t = T(BT::UInt, n[4] - '0');
        else if (n.size() == 5 && n.rfind("bvec", 0) == 0) t = T(BT::Bool, n[4] - '0');
        else if (n.rfind("mat", 0) == 0) {
            const int c = n[3] - '0';
            const int r = n.size() > 5 ? n[5] - '0' : c;
            t = T(BT::Float, r, c);
        } else if (n == "sampler2D" || n == "samplerExternalOES") {
            t = T(BT::Sampler);
        } else if (n.find("sampler") != std::string::npos) {
            error(ts.loc, "sampler type '" + n + "' is not supported (only sampler2D)");
        } else {
            auto it = struct_by_name_.find(n);
            if (it == struct_by_name_.end()) error(ts.loc, "unknown type '" + n + "'");
            t.bt = BT::Struct;
            t.st = it->second;
        }
        if (ts.is_array) {
            if (!ts.array_size) error(ts.loc, "unsized arrays are not supported");
            t.array = const_int(*ts.array_size, "array size");
            if (t.array <= 0) error(ts.loc, "array size must be positive");
        }
        return t;
    }

    int define_struct(const StructDecl &sd)
    {
        auto it = struct_by_decl_.find(&sd);
        if (it != struct_by_decl_.end()) return it->second;
        SDef d;
        d.name = sd.name;
        for (const auto &m : sd.members) {
            Ty mt = resolve(m.type);
            if (m.is_array) {
                if (!m.array_size) error(m.loc, "unsized arrays are not supported");
                mt.array = const_int(*m.array_size, "array size");
            }
            if (mt.bt == BT::Sampler) error(m.loc, "samplers inside structs are not supported");
            d.members.push_back({m.name, mt});
        }
        structs_.push_back(d);
        const int id = static_cast<int>(structs_.size()) - 1;
        struct_by_decl_[&sd] = id;
        struct_by_name_[sd.name] = id;
        return id;
    }

    int const_int(const Expr &e, const char *what)
    {
        Val v = rval(e);
        float f;
        if (!v.ty.is_scalar() || !v.ty.is_int() || !isK(v.s[0], &f)) {
            error(e.loc, std::string(what) + " must be a constant integer expression");
        }
        return static_cast<int>(f);
    }

    /* ── 作用域 ── */

    VarP lookup(const std::string &n)
    {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) return f->second;
        }
        return nullptr;
    }

    void declare(const std::string &n, VarP v, SrcLoc l)
    {
        if (scopes_.back().count(n)) error(l, "redefinition of '" + n + "'");
        scopes_.back()[n] = std::move(v);
    }

    std::vector<Scalar> zeros(const Ty &t)
    {
        return std::vector<Scalar>(static_cast<size_t>(count(t)), K(0));
    }

    /* ── 赋值（谓词化） ── */

    void write(Var &v, const std::vector<int> &pos, const std::vector<Scalar> &val, Scalar cond)
    {
        /* for 初始化里声明的循环变量离开循环就不可见；谓词等于"本循环仍存活"时，
         * 不存活的片元写什么都无所谓，于是可以不加谓词——计数器因此保持常量可折叠 */
        Scalar base = pred_;
        if (v.loop_id >= 0 && v.loop_id == cur_loop_id_ && pred_ == cur_loop_lp_) base = K(1);
        Scalar p = pand(base, cond);
        float pv;
        if (isK(p, &pv) && pv == 0) return;
        if (isK(p, &pv) && pv != 0) {
            for (size_t i = 0; i < pos.size(); i++) v.s[static_cast<size_t>(pos[i])] = val[i];
            return;
        }
        std::vector<Scalar> oldv;
        for (int q : pos) oldv.push_back(v.s[static_cast<size_t>(q)]);
        auto r = cw(ESPGPU_VM_SEL, splat(p, pos.size()), val, oldv);
        for (size_t i = 0; i < pos.size(); i++) v.s[static_cast<size_t>(pos[i])] = r[i];
    }

    void assign(LVal &lv, const Val &v, SrcLoc l)
    {
        if (lv.var->readonly) error(l, "cannot assign to " + lv.var->what);
        if (lv.swizzle_dup) error(l, "l-value swizzle has repeated components");
        if (lv.ty != v.ty) error(l, "cannot assign '" + tname(v.ty) + "' to '" + tname(lv.ty) + "'");
        if (lv.ty.bt == BT::Sampler) error(l, "cannot assign to a sampler");
        for (auto &a : lv.alts) write(*lv.var, a.pos, v.s, a.cond);
    }

    Val read(const LVal &lv)
    {
        Val r;
        r.ty = lv.ty;
        if (lv.ty.bt == BT::Sampler) {
            for (int i : lv.samp_idx) r.samp.push_back(lv.var->samp[static_cast<size_t>(i)]);
            return r;
        }
        const Alt &a0 = lv.alts[0];
        for (int q : a0.pos) r.s.push_back(lv.var->s[static_cast<size_t>(q)]);
        for (size_t k = 1; k < lv.alts.size(); k++) {
            std::vector<Scalar> o;
            for (int q : lv.alts[k].pos) o.push_back(lv.var->s[static_cast<size_t>(q)]);
            r.s = cw(ESPGPU_VM_SEL, splat(lv.alts[k].cond, o.size()), o, r.s);
        }
        return r;
    }

    /* ── 表达式 ── */

    static bool swz_index(char c, int &idx, int &set)
    {
        const char *sets[3] = {"xyzw", "rgba", "stpq"};
        for (int s = 0; s < 3; s++) {
            const char *p = std::strchr(sets[s], c);
            if (p && *p) {
                idx = static_cast<int>(p - sets[s]);
                set = s;
                return true;
            }
        }
        return false;
    }

    /** 成员 / 下标的偏移（以标量计）与类型 */
    int member_offset(const Ty &st, const std::string &name, Ty &mt, SrcLoc l)
    {
        int off = 0;
        for (const auto &m : structs_[static_cast<size_t>(st.st)].members) {
            if (m.first == name) {
                mt = m.second;
                return off;
            }
            off += count(m.second);
        }
        error(l, "'" + tname(st) + "' has no member '" + name + "'");
    }

    bool lval(const Expr &e, LVal &out)
    {
        switch (e.kind) {
        case Expr::Ident: {
            if (stage_ == Stage::Fragment && e.name == "gl_FragCoord" && !lookup(e.name)) make_fragcoord();
            VarP v = lookup(e.name);
            if (!v) error(e.loc, "undeclared identifier '" + e.name + "'");
            out.var = v;
            out.ty = v->ty;
            Alt a;
            a.cond = K(1);
            for (int i = 0; i < count(v->ty); i++) a.pos.push_back(i);
            out.alts = {a};
            for (int i = 0; i < samp_count(v->ty); i++) out.samp_idx.push_back(i);
            return true;
        }
        case Expr::Field: {
            if (!lval(*e.args[0], out)) return false;
            const Ty bt = out.ty;
            if (e.name == "length()") return false;
            if (bt.bt == BT::Struct && !bt.is_array()) {
                Ty mt;
                const int off = member_offset(bt, e.name, mt, e.loc);
                const int n = count(mt);
                for (auto &a : out.alts) {
                    std::vector<int> np(a.pos.begin() + off, a.pos.begin() + off + n);
                    a.pos = np;
                }
                out.ty = mt;
                return true;
            }
            if ((bt.is_vector() || bt.is_scalar()) && (bt.numeric() || bt.bt == BT::Bool)) {
                std::vector<int> sel;
                int set0 = -1;
                std::set<int> seen;
                for (char c : e.name) {
                    int idx, set;
                    if (!swz_index(c, idx, set) || idx >= bt.vec) {
                        error(e.loc, "invalid swizzle '" + e.name + "' on '" + tname(bt) + "'");
                    }
                    if (set0 >= 0 && set != set0) error(e.loc, "swizzle mixes component sets");
                    set0 = set;
                    if (!seen.insert(idx).second) out.swizzle_dup = true;
                    sel.push_back(idx);
                }
                if (sel.size() > 4) error(e.loc, "swizzle too long");
                for (auto &a : out.alts) {
                    std::vector<int> np;
                    for (int s : sel) np.push_back(a.pos[static_cast<size_t>(s)]);
                    a.pos = np;
                }
                out.ty = bt.with_vec(static_cast<int>(sel.size()));
                return true;
            }
            error(e.loc, "request for member '" + e.name + "' of non-struct type '" + tname(bt) + "'");
        }
        case Expr::Index: {
            if (!lval(*e.args[0], out)) return false;
            const Ty bt = out.ty;
            Ty et;
            int n;
            int stride;
            if (bt.is_array()) {
                et = bt.elem();
                n = bt.array;
                stride = count(et);
            } else if (bt.is_matrix()) {
                et = bt.column();
                n = bt.cols;
                stride = bt.vec;
            } else if (bt.is_vector()) {
                et = bt.with_vec(1);
                n = bt.vec;
                stride = 1;
            } else {
                error(e.loc, "subscripted value '" + tname(bt) + "' is not an array, vector or matrix");
            }
            Val iv = rval(*e.args[1]);
            if (!iv.ty.is_scalar() || !iv.ty.is_int()) error(e.args[1]->loc, "array index must be an integer");
            float k;
            if (et.bt == BT::Sampler) {
                if (!isK(iv.s[0], &k)) error(e.loc, "sampler arrays need constant indices");
                if (k < 0 || k >= n) error(e.loc, "array index out of range");
                out.samp_idx = {out.samp_idx[static_cast<size_t>(k)]};
                out.ty = et;
                return true;
            }
            if (isK(iv.s[0], &k)) {
                const int ki = static_cast<int>(k);
                if (ki < 0 || ki >= n) error(e.loc, "index " + std::to_string(ki) + " out of range");
                for (auto &a : out.alts) {
                    std::vector<int> np(a.pos.begin() + ki * stride, a.pos.begin() + (ki + 1) * stride);
                    a.pos = np;
                }
            } else {
                std::vector<Alt> na;
                for (auto &a : out.alts) {
                    for (int ki = 0; ki < n; ki++) {
                        Alt b;
                        b.cond = pand(a.cond, cw1(ESPGPU_VM_SEQ, iv.s[0], K(static_cast<float>(ki))));
                        b.pos.assign(a.pos.begin() + ki * stride, a.pos.begin() + (ki + 1) * stride);
                        na.push_back(b);
                    }
                }
                out.alts = na;
            }
            out.ty = et;
            return true;
        }
        default:
            return false;
        }
    }

    Val rval(const Expr &e)
    {
        switch (e.kind) {
        case Expr::IntLit:
            return {T(BT::Int), {K(static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(e.num)))))}, {}};
        case Expr::UIntLit: return {T(BT::UInt), {K(static_cast<float>(e.num))}, {}};
        case Expr::FloatLit: return {T(BT::Float), {K(static_cast<float>(e.num))}, {}};
        case Expr::BoolLit: return {T(BT::Bool), {K(e.num != 0 ? 1.0f : 0.0f)}, {}};
        case Expr::Ident:
        case Expr::Index:
        case Expr::Field: {
            if (e.kind == Expr::Field && e.name == "length()") {
                Val b = rval(*e.args[0]);
                if (!b.ty.is_array()) error(e.loc, "length() requires an array");
                return {T(BT::Int), {K(static_cast<float>(b.ty.array))}, {}};
            }
            LVal lv;
            if (lval(e, lv)) return read(lv);
            /* 基值不是左值（如 (a+b).xy / f()[1]）：先求值再放进临时变量 */
            Val base = rval(*e.args[0]);
            auto tmp = std::make_shared<Var>();
            tmp->ty = base.ty;
            tmp->s = base.s;
            tmp->samp = base.samp;
            LVal l2;
            l2.var = tmp;
            l2.ty = tmp->ty;
            Alt a;
            a.cond = K(1);
            for (int i = 0; i < count(tmp->ty); i++) a.pos.push_back(i);
            l2.alts = {a};
            for (int i = 0; i < samp_count(tmp->ty); i++) l2.samp_idx.push_back(i);
            return rval_on_temp(e, l2);
        }
        case Expr::Unary: return unary(e);
        case Expr::Binary: return binary(e);
        case Expr::Assign: return assign_expr(e);
        case Expr::PreInc:
        case Expr::PostInc: return incdec(e);
        case Expr::Ternary: {
            Val c = rval(*e.args[0]);
            if (c.ty != T(BT::Bool)) error(e.args[0]->loc, "ternary condition must be a bool");
            /* 两侧都求值；有副作用时按谓词约束 */
            const Scalar save = pred_;
            pred_ = pand(save, c.s[0]);
            Val a = rval(*e.args[1]);
            pred_ = pand(save, pnot(c.s[0]));
            Val b = rval(*e.args[2]);
            pred_ = save;
            if (a.ty != b.ty) error(e.loc, "ternary branches have different types");
            if (a.ty.bt == BT::Sampler) {
                float k;
                if (!isK(c.s[0], &k)) error(e.loc, "sampler selection needs a constant condition");
                return k != 0 ? a : b;
            }
            Val r;
            r.ty = a.ty;
            r.s = cw(ESPGPU_VM_SEL, splat(c.s[0], a.s.size()), a.s, b.s);
            return r;
        }
        case Expr::Comma: {
            rval(*e.args[0]);
            return rval(*e.args[1]);
        }
        case Expr::Call: return call(e);
        }
        error(e.loc, "unsupported expression");
    }

    /* 对非左值基表达式施加 Field/Index */
    Val rval_on_temp(const Expr &e, LVal &l)
    {
        const Ty bt = l.ty;
        if (e.kind == Expr::Field) {
            if (bt.bt == BT::Struct && !bt.is_array()) {
                Ty mt;
                const int off = member_offset(bt, e.name, mt, e.loc);
                std::vector<int> np(l.alts[0].pos.begin() + off, l.alts[0].pos.begin() + off + count(mt));
                l.alts[0].pos = np;
                l.ty = mt;
                return read(l);
            }
            if (!bt.is_vector() && !bt.is_scalar()) error(e.loc, "invalid member access on '" + tname(bt) + "'");
            std::vector<int> np;
            int set0 = -1;
            for (char c : e.name) {
                int idx, set;
                if (!swz_index(c, idx, set) || idx >= bt.vec) error(e.loc, "invalid swizzle '" + e.name + "'");
                if (set0 >= 0 && set != set0) error(e.loc, "swizzle mixes component sets");
                set0 = set;
                np.push_back(l.alts[0].pos[static_cast<size_t>(idx)]);
            }
            if (np.size() > 4) error(e.loc, "swizzle too long");
            l.alts[0].pos = np;
            l.ty = bt.with_vec(static_cast<int>(np.size()));
            return read(l);
        }
        /* Index */
        Ty et;
        int n, stride;
        if (bt.is_array()) {
            et = bt.elem();
            n = bt.array;
            stride = count(et);
        } else if (bt.is_matrix()) {
            et = bt.column();
            n = bt.cols;
            stride = bt.vec;
        } else if (bt.is_vector()) {
            et = bt.with_vec(1);
            n = bt.vec;
            stride = 1;
        } else {
            error(e.loc, "subscripted value is not an array, vector or matrix");
        }
        Val iv = rval(*e.args[1]);
        if (!iv.ty.is_scalar() || !iv.ty.is_int()) error(e.args[1]->loc, "array index must be an integer");
        float k;
        if (isK(iv.s[0], &k)) {
            const int ki = static_cast<int>(k);
            if (ki < 0 || ki >= n) error(e.loc, "index out of range");
            std::vector<int> np(l.alts[0].pos.begin() + ki * stride, l.alts[0].pos.begin() + (ki + 1) * stride);
            l.alts[0].pos = np;
        } else {
            std::vector<Alt> na;
            for (int ki = 0; ki < n; ki++) {
                Alt b;
                b.cond = cw1(ESPGPU_VM_SEQ, iv.s[0], K(static_cast<float>(ki)));
                b.pos.assign(l.alts[0].pos.begin() + ki * stride, l.alts[0].pos.begin() + (ki + 1) * stride);
                na.push_back(b);
            }
            l.alts = na;
        }
        l.ty = et;
        return read(l);
    }

    /* 整数截断：sign(x) * floor(|x|) */
    std::vector<Scalar> trunc_v(const std::vector<Scalar> &x)
    {
        auto f = cw(ESPGPU_VM_FLR, cw(ESPGPU_VM_ABS, x));
        return cw(ESPGPU_VM_MUL, cw(ESPGPU_VM_SIGN, x), f);
    }

    /** 广播：把标量操作数展开到另一侧的长度 */
    void broadcast(Val &a, Val &b, SrcLoc l, const std::string &op)
    {
        if (a.ty.bt != b.ty.bt) {
            error(l, "no operation '" + op + "' for '" + tname(a.ty) + "' and '" + tname(b.ty) +
                         "' (GLSL ES has no implicit conversions)");
        }
        if (a.ty.is_array() || b.ty.is_array() || a.ty.bt == BT::Struct || b.ty.bt == BT::Struct) {
            error(l, "operator '" + op + "' needs scalar, vector or matrix operands");
        }
        if (a.ty.is_scalar() && !b.ty.is_scalar()) {
            a.s = splat(a.s[0], b.s.size());
            a.ty = b.ty;
        } else if (b.ty.is_scalar() && !a.ty.is_scalar()) {
            b.s = splat(b.s[0], a.s.size());
            b.ty = a.ty;
        } else if (a.ty != b.ty) {
            error(l, "no operation '" + op + "' for '" + tname(a.ty) + "' and '" + tname(b.ty) + "'");
        }
    }

    Val arith(const std::string &op, Val a, Val b, SrcLoc l)
    {
        if (!a.ty.numeric() || !b.ty.numeric()) {
            error(l, "operator '" + op + "' needs numeric operands, got '" + tname(a.ty) + "' and '" +
                         tname(b.ty) + "'");
        }
        if (op == "*" && (a.ty.is_matrix() || b.ty.is_matrix()) && !a.ty.is_scalar() &&
            !b.ty.is_scalar()) {
            return matmul(a, b, l);
        }
        broadcast(a, b, l, op);
        Val r;
        r.ty = a.ty;
        if (op == "+") r.s = cw(ESPGPU_VM_ADD, a.s, b.s);
        else if (op == "-") r.s = cw(ESPGPU_VM_SUB, a.s, b.s);
        else if (op == "*") r.s = cw(ESPGPU_VM_MUL, a.s, b.s);
        else if (op == "/") {
            r.s = cw(ESPGPU_VM_DIV, a.s, b.s);
            if (a.ty.is_int()) {
                for (auto s : b.s) {
                    if (isK(s, 0.0f)) error(l, "integer division by zero");
                }
                r.s = trunc_v(r.s);
            }
        } else if (op == "%") {
            if (!a.ty.is_int()) error(l, "operator '%' needs integer operands");
            auto q = trunc_v(cw(ESPGPU_VM_DIV, a.s, b.s));
            r.s = cw(ESPGPU_VM_SUB, a.s, cw(ESPGPU_VM_MUL, b.s, q));
        } else {
            error(l, "unknown operator '" + op + "'");
        }
        return r;
    }

    std::vector<Scalar> col(const Val &m, int c) const
    {
        return std::vector<Scalar>(m.s.begin() + c * m.ty.vec, m.s.begin() + (c + 1) * m.ty.vec);
    }

    /** M * v：结果 = Σ col_i * v_i，按 MUL + MAD 链展开 */
    std::vector<Scalar> mat_vec(const Val &m, const std::vector<Scalar> &v)
    {
        std::vector<Scalar> acc = cw(ESPGPU_VM_MUL, col(m, 0), splat(v[0], static_cast<size_t>(m.ty.vec)));
        for (int i = 1; i < m.ty.cols; i++) {
            acc = cw(ESPGPU_VM_MAD, col(m, i), splat(v[static_cast<size_t>(i)], static_cast<size_t>(m.ty.vec)), acc);
        }
        return acc;
    }

    Val matmul(const Val &a, const Val &b, SrcLoc l)
    {
        if (a.ty.bt != BT::Float || b.ty.bt != BT::Float) error(l, "matrix product needs float operands");
        if (a.ty.is_matrix() && b.ty.is_vector()) {
            if (a.ty.cols != b.ty.vec) error(l, "matrix/vector size mismatch in '*'");
            return {T(BT::Float, a.ty.vec), mat_vec(a, b.s), {}};
        }
        if (a.ty.is_vector() && b.ty.is_matrix()) {
            if (a.ty.vec != b.ty.vec) error(l, "vector/matrix size mismatch in '*'");
            Val r;
            r.ty = T(BT::Float, b.ty.cols);
            for (int c = 0; c < b.ty.cols; c++) r.s.push_back(dot(a.s, col(b, c)));
            return r;
        }
        if (a.ty.is_matrix() && b.ty.is_matrix()) {
            if (a.ty.cols != b.ty.vec) error(l, "matrix size mismatch in '*'");
            Val r;
            r.ty = T(BT::Float, a.ty.vec, b.ty.cols);
            for (int c = 0; c < b.ty.cols; c++) {
                auto cc = mat_vec(a, col(b, c));
                r.s.insert(r.s.end(), cc.begin(), cc.end());
            }
            return r;
        }
        error(l, "invalid operands to '*'");
    }

    Val compare_eq(const Val &a, const Val &b, bool eq, SrcLoc l)
    {
        if (a.ty != b.ty) error(l, "cannot compare '" + tname(a.ty) + "' with '" + tname(b.ty) + "'");
        if (a.ty.bt == BT::Sampler) error(l, "cannot compare samplers");
        auto e = cw(ESPGPU_VM_SEQ, a.s, b.s);
        Scalar all = K(1);
        for (auto s : e) all = pand(all, s);
        return {T(BT::Bool), {eq ? all : pnot(all)}, {}};
    }

    Val binary(const Expr &e)
    {
        const std::string &op = e.op;
        if (op == "&&" || op == "||" || op == "^^") {
            Val a = rval(*e.args[0]);
            if (a.ty != T(BT::Bool)) error(e.args[0]->loc, "operand of '" + op + "' must be a bool");
            /* 短路：右侧的副作用只在需要时发生 */
            const Scalar save = pred_;
            if (op == "&&") pred_ = pand(save, a.s[0]);
            else if (op == "||") pred_ = pand(save, pnot(a.s[0]));
            Val b = rval(*e.args[1]);
            pred_ = save;
            if (b.ty != T(BT::Bool)) error(e.args[1]->loc, "operand of '" + op + "' must be a bool");
            Scalar r;
            if (op == "&&") r = pand(a.s[0], b.s[0]);
            else if (op == "||") r = cw1(ESPGPU_VM_MAX, a.s[0], b.s[0]);
            else r = cw1(ESPGPU_VM_SNE, a.s[0], b.s[0]);
            return {T(BT::Bool), {r}, {}};
        }
        Val a = rval(*e.args[0]);
        Val b = rval(*e.args[1]);
        if (op == "==" || op == "!=") return compare_eq(a, b, op == "==", e.loc);
        if (op == "<" || op == ">" || op == "<=" || op == ">=") {
            if (!a.ty.is_scalar() || !b.ty.is_scalar() || !a.ty.numeric() || a.ty != b.ty) {
                error(e.loc, "relational operator '" + op + "' needs two scalars of the same numeric type");
            }
            Scalar r;
            if (op == "<") r = cw1(ESPGPU_VM_SLT, a.s[0], b.s[0]);
            else if (op == ">") r = cw1(ESPGPU_VM_SLT, b.s[0], a.s[0]);
            else if (op == "<=") r = cw1(ESPGPU_VM_SGE, b.s[0], a.s[0]);
            else r = cw1(ESPGPU_VM_SGE, a.s[0], b.s[0]);
            return {T(BT::Bool), {r}, {}};
        }
        if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
            if (!a.ty.is_int() || !b.ty.is_int()) error(e.loc, "bitwise operator needs integer operands");
            broadcast(a, b, e.loc, op);
            Val r;
            r.ty = a.ty;
            for (size_t i = 0; i < a.s.size(); i++) {
                float x, y;
                if (!isK(a.s[i], &x) || !isK(b.s[i], &y)) {
                    error(e.loc, "bitwise operators are only supported on constant expressions");
                }
                const int64_t xi = static_cast<int64_t>(x), yi = static_cast<int64_t>(y);
                int64_t v = 0;
                if (op == "&") v = xi & yi;
                else if (op == "|") v = xi | yi;
                else if (op == "^") v = xi ^ yi;
                else if (op == "<<") v = xi << (yi & 31);
                else v = xi >> (yi & 31);
                r.s.push_back(K(static_cast<float>(a.ty.bt == BT::UInt ? static_cast<uint32_t>(v) : static_cast<int32_t>(v))));
            }
            return r;
        }
        return arith(op, a, b, e.loc);
    }

    Val unary(const Expr &e)
    {
        Val a = rval(*e.args[0]);
        if (e.op == "+") {
            if (!a.ty.numeric()) error(e.loc, "unary '+' needs a numeric operand");
            return a;
        }
        if (e.op == "-") {
            if (!a.ty.numeric() || a.ty.is_array() || a.ty.bt == BT::Struct) error(e.loc, "unary '-' needs a numeric operand");
            Val r = a;
            r.s = cw(ESPGPU_VM_SUB, splat(K(0), a.s.size()), a.s);
            return r;
        }
        if (e.op == "!") {
            if (a.ty != T(BT::Bool)) error(e.loc, "'!' needs a bool operand");
            return {a.ty, {pnot(a.s[0])}, {}};
        }
        if (e.op == "~") {
            if (!a.ty.is_int()) error(e.loc, "'~' needs an integer operand");
            Val r = a;
            for (auto &s : r.s) {
                float v;
                if (!isK(s, &v)) error(e.loc, "'~' is only supported on constants");
                s = K(static_cast<float>(~static_cast<int32_t>(v)));
            }
            return r;
        }
        error(e.loc, "unknown unary operator");
    }

    Val assign_expr(const Expr &e)
    {
        LVal lv;
        if (!lval(*e.args[0], lv)) error(e.args[0]->loc, "left side of assignment is not an l-value");
        Val rhs = rval(*e.args[1]);
        if (e.op != "=") {
            Val cur = read(lv);
            const std::string op = e.op.substr(0, e.op.size() - 1);
            if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
                error(e.loc, "compound bitwise assignment is not supported");
            }
            rhs = arith(op, cur, rhs, e.loc);
        }
        assign(lv, rhs, e.loc);
        return rhs;
    }

    Val incdec(const Expr &e)
    {
        LVal lv;
        if (!lval(*e.args[0], lv)) error(e.loc, "operand of '" + e.op + "' is not an l-value");
        Val cur = read(lv);
        if (!cur.ty.numeric() || cur.ty.is_array()) error(e.loc, "'" + e.op + "' needs a numeric operand");
        Val one{cur.ty.elem().with_vec(1), {K(1)}, {}};
        one.ty.bt = cur.ty.bt;
        Val nv = arith(e.op == "++" ? "+" : "-", cur, one, e.loc);
        assign(lv, nv, e.loc);
        return e.kind == Expr::PreInc ? nv : cur;
    }

    /* ── 构造函数 ── */

    Scalar convert(Scalar s, BT from, BT to)
    {
        if (from == to) return s;
        if (to == BT::Bool) return cw1(ESPGPU_VM_SNE, s, K(0));
        if (to == BT::Float) return s; /* int/uint/bool → float 数值相同 */
        if (from == BT::Float) return trunc_v({s})[0];
        return s; /* int ↔ uint ↔ bool→int */
    }

    std::vector<Scalar> flatten_args(const std::vector<Val> &args, BT to)
    {
        std::vector<Scalar> out;
        for (const auto &a : args) {
            for (auto s : a.s) out.push_back(convert(s, a.ty.bt, to));
        }
        return out;
    }

    Val construct(const Ty &t, const std::vector<Val> &args, SrcLoc l)
    {
        for (const auto &a : args) {
            if (a.ty.bt == BT::Sampler || a.ty.bt == BT::Void) error(l, "invalid constructor argument");
        }
        if (t.is_array()) {
            if (static_cast<int>(args.size()) != t.array) error(l, "array constructor needs " + std::to_string(t.array) + " elements");
            Val r;
            r.ty = t;
            for (const auto &a : args) {
                if (a.ty != t.elem()) error(l, "array constructor element type mismatch");
                r.s.insert(r.s.end(), a.s.begin(), a.s.end());
            }
            return r;
        }
        if (t.bt == BT::Struct) {
            const auto &m = structs_[static_cast<size_t>(t.st)].members;
            if (args.size() != m.size()) error(l, "struct constructor needs " + std::to_string(m.size()) + " arguments");
            Val r;
            r.ty = t;
            for (size_t i = 0; i < m.size(); i++) {
                if (args[i].ty != m[i].second) error(l, "struct constructor argument " + std::to_string(i + 1) + " has the wrong type");
                r.s.insert(r.s.end(), args[i].s.begin(), args[i].s.end());
            }
            return r;
        }
        if (args.empty()) error(l, "constructor needs arguments");
        for (const auto &a : args) {
            if (a.ty.is_array() || a.ty.bt == BT::Struct) error(l, "invalid constructor argument type '" + tname(a.ty) + "'");
        }
        const int n = t.vec * t.cols;
        Val r;
        r.ty = t;
        if (t.is_matrix()) {
            if (args.size() == 1 && args[0].ty.is_scalar()) {
                const Scalar d = convert(args[0].s[0], args[0].ty.bt, BT::Float);
                for (int c = 0; c < t.cols; c++)
                    for (int rr = 0; rr < t.vec; rr++) r.s.push_back(c == rr ? d : K(0));
                return r;
            }
            if (args.size() == 1 && args[0].ty.is_matrix()) {
                const Val &m = args[0];
                for (int c = 0; c < t.cols; c++)
                    for (int rr = 0; rr < t.vec; rr++) {
                        if (c < m.ty.cols && rr < m.ty.vec) r.s.push_back(m.s[static_cast<size_t>(c * m.ty.vec + rr)]);
                        else r.s.push_back(K(c == rr ? 1.0f : 0.0f));
                    }
                return r;
            }
            auto f = flatten_args(args, BT::Float);
            if (static_cast<int>(f.size()) != n) error(l, "wrong number of components for '" + tname(t) + "' constructor");
            r.s = f;
            return r;
        }
        if (args.size() == 1 && args[0].ty.is_scalar()) {
            r.s = splat(convert(args[0].s[0], args[0].ty.bt, t.bt), static_cast<size_t>(n));
            return r;
        }
        auto f = flatten_args(args, t.bt);
        if (static_cast<int>(f.size()) < n) error(l, "not enough components for '" + tname(t) + "' constructor");
        /* 最后一个参数允许部分使用，再多余就是错误 */
        size_t used = 0;
        size_t ai = 0;
        for (; ai < args.size() && static_cast<int>(used) < n; ai++) used += args[ai].s.size();
        if (ai < args.size()) error(l, "too many arguments to '" + tname(t) + "' constructor");
        f.resize(static_cast<size_t>(n));
        r.s = f;
        return r;
    }

    /* ── 内建函数 ── */

    static bool gen_float(const Ty &t) { return t.bt == BT::Float && (t.is_scalar() || t.is_vector()); }

    void need(bool ok, SrcLoc l, const std::string &name, const std::vector<Val> &a)
    {
        if (ok) return;
        std::string sig = name + "(";
        for (size_t i = 0; i < a.size(); i++) sig += (i ? ", " : "") + tname(a[i].ty);
        error(l, "no matching overload for " + sig + ")");
    }

    /** 第二个参数允许是 float 而第一个是 vecN（mod/min/max/clamp/step/mix 的重载） */
    std::vector<Scalar> widen(const Val &v, size_t n) { return v.ty.is_scalar() ? splat(v.s[0], n) : v.s; }

    std::vector<Scalar> atan_core(const std::vector<Scalar> &x)
    {
        /* |x| <= 1 的多项式近似，误差 < 1e-5 */
        auto x2 = cw(ESPGPU_VM_MUL, x, x);
        const size_t n = x.size();
        auto p = splat(K(0.0208351f), n);
        p = cw(ESPGPU_VM_MAD, p, x2, splat(K(-0.0851330f), n));
        p = cw(ESPGPU_VM_MAD, p, x2, splat(K(0.1801410f), n));
        p = cw(ESPGPU_VM_MAD, p, x2, splat(K(-0.3302995f), n));
        p = cw(ESPGPU_VM_MAD, p, x2, splat(K(0.9998660f), n));
        return cw(ESPGPU_VM_MUL, p, x);
    }

    std::vector<Scalar> atan2_v(const std::vector<Scalar> &y, const std::vector<Scalar> &x)
    {
        const size_t n = y.size();
        auto ax = cw(ESPGPU_VM_ABS, x);
        auto ay = cw(ESPGPU_VM_ABS, y);
        auto mx = cw(ESPGPU_VM_MAX, ax, ay);
        auto mn = cw(ESPGPU_VM_MIN, ax, ay);
        /* mx 为 0 时 VM 的 DIV 定义为 0（eval_op 与设备一致），atan2(0, 0) = 0 */
        auto t = cw(ESPGPU_VM_DIV, mn, mx);
        auto a = atan_core(t);
        auto swap = cw(ESPGPU_VM_SLT, ax, ay);
        a = cw(ESPGPU_VM_SEL, swap, cw(ESPGPU_VM_SUB, splat(K(1.5707963f), n), a), a);
        auto xneg = cw(ESPGPU_VM_SLT, x, splat(K(0), n));
        a = cw(ESPGPU_VM_SEL, xneg, cw(ESPGPU_VM_SUB, splat(K(3.1415927f), n), a), a);
        auto yneg = cw(ESPGPU_VM_SLT, y, splat(K(0), n));
        return cw(ESPGPU_VM_SEL, yneg, cw(ESPGPU_VM_SUB, splat(K(0), n), a), a);
    }

    std::vector<Scalar> exp_v(const std::vector<Scalar> &x)
    {
        return cw(ESPGPU_VM_EXP2, cw(ESPGPU_VM_MUL, x, splat(K(1.4426950f), x.size())));
    }

    Scalar length_s(const std::vector<Scalar> &v)
    {
        if (v.size() == 1) return cw1(ESPGPU_VM_ABS, v[0]);
        return cw1(ESPGPU_VM_SQRT, dot(v, v));
    }

    Val texture_call(const std::string &name, std::vector<Val> &a, SrcLoc l)
    {
        if (stage_ != Stage::Fragment) error(l, "texture lookups in vertex shaders are not supported");
        if (a.size() < 2 || a[0].ty.bt != BT::Sampler || a[0].ty.is_array()) error(l, name + "() needs a sampler2D as first argument");
        const Val &c = a[1];
        std::vector<Scalar> uv;
        const bool proj = name == "textureProj" || name == "texture2DProj" || name == "texture2DProjLod" || name == "textureProjLod";
        if (proj) {
            if (c.ty != T(BT::Float, 3) && c.ty != T(BT::Float, 4)) error(l, name + "() needs vec3 or vec4 coordinates");
            const Scalar q = c.s[static_cast<size_t>(c.ty.vec - 1)];
            uv = cw(ESPGPU_VM_DIV, {c.s[0], c.s[1]}, {q, q});
        } else {
            if (c.ty != T(BT::Float, 2)) error(l, name + "() needs vec2 coordinates");
            uv = c.s;
        }
        /* lod / bias 参数被忽略（设备没有 mipmap） */
        Node nd;
        nd.kind = NK::Tex;
        nd.width = 4;
        nd.nargs = 1;
        nd.a[0] = operand(uv, 0, 2);
        nd.index = a[0].samp[0];
        const int id = add_node(nd);
        return {T(BT::Float, 4), outs(id, 4), {}};
    }

    bool builtin(const std::string &name, std::vector<Val> &a, SrcLoc l, Val &out)
    {
        const size_t na = a.size();
        auto f1 = [&](uint8_t op) {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            out = {a[0].ty, cw(op, a[0].s), {}};
        };
        auto scale = [&](float k) {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            out = {a[0].ty, cw(ESPGPU_VM_MUL, a[0].s, splat(K(k), a[0].s.size())), {}};
        };

        if (name == "texture" || name == "texture2D" || name == "textureProj" || name == "texture2DProj" ||
            name == "textureLod" || name == "texture2DLod" || name == "texture2DProjLod" ||
            name == "textureProjLod" || name == "textureGrad") {
            out = texture_call(name, a, l);
            return true;
        }
        if (name == "textureSize" || name == "texelFetch" || name == "textureOffset" ||
            name == "textureCube" || name == "dFdx" || name == "dFdy" || name == "fwidth" ||
            name == "inverse" || name == "modf" || name == "floatBitsToInt" || name == "intBitsToFloat" ||
            name == "packHalf2x16" || name == "unpackHalf2x16") {
            error(l, "built-in function '" + name + "' is not supported on this device");
        }
        if (name == "radians") { scale(0.017453293f); return true; }
        if (name == "degrees") { scale(57.29578f); return true; }
        if (name == "sin") { f1(ESPGPU_VM_SIN); return true; }
        if (name == "cos") { f1(ESPGPU_VM_COS); return true; }
        if (name == "tan") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            out = {a[0].ty, cw(ESPGPU_VM_DIV, cw(ESPGPU_VM_SIN, a[0].s), cw(ESPGPU_VM_COS, a[0].s)), {}};
            return true;
        }
        if (name == "atan") {
            if (na == 2) {
                need(gen_float(a[0].ty) && a[0].ty == a[1].ty, l, name, a);
                out = {a[0].ty, atan2_v(a[0].s, a[1].s), {}};
            } else {
                need(na == 1 && gen_float(a[0].ty), l, name, a);
                out = {a[0].ty, atan2_v(a[0].s, splat(K(1), a[0].s.size())), {}};
            }
            return true;
        }
        if (name == "asin" || name == "acos") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            const size_t n = a[0].s.size();
            auto x = a[0].s;
            auto s = cw(ESPGPU_VM_SQRT, cw(ESPGPU_VM_MAX, splat(K(0), n),
                                           cw(ESPGPU_VM_SUB, splat(K(1), n), cw(ESPGPU_VM_MUL, x, x))));
            out = {a[0].ty, name == "asin" ? atan2_v(x, s) : atan2_v(s, x), {}};
            return true;
        }
        if (name == "sinh" || name == "cosh" || name == "tanh") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            const size_t n = a[0].s.size();
            auto ep = exp_v(a[0].s);
            auto en = exp_v(cw(ESPGPU_VM_SUB, splat(K(0), n), a[0].s));
            auto sh = cw(ESPGPU_VM_MUL, cw(ESPGPU_VM_SUB, ep, en), splat(K(0.5f), n));
            auto ch = cw(ESPGPU_VM_MUL, cw(ESPGPU_VM_ADD, ep, en), splat(K(0.5f), n));
            out = {a[0].ty, name == "sinh" ? sh : name == "cosh" ? ch : cw(ESPGPU_VM_DIV, sh, ch), {}};
            return true;
        }
        if (name == "pow") {
            need(na == 2 && gen_float(a[0].ty) && a[0].ty == a[1].ty, l, name, a);
            /* 常量小整数指数换成乘法（设备上 powf 是几百个周期的库函数）；
             * x < 0 时 GLSL 的 pow 未定义，这里的结果与 POW 的"负底数按 0"不同也无妨 */
            bool all_k = true;
            float e0 = 0;
            for (size_t i = 0; i < a[1].s.size(); i++) {
                float e = 0;
                all_k = all_k && isK(a[1].s[i], &e) && (i == 0 || e == e0);
                if (i == 0) e0 = e;
            }
            if (all_k && (e0 == 1.0f || e0 == 2.0f || e0 == 3.0f || e0 == 4.0f)) {
                const auto &x = a[0].s;
                std::vector<Scalar> r = x;
                if (e0 >= 2.0f) r = cw(ESPGPU_VM_MUL, x, x);
                if (e0 == 3.0f) r = cw(ESPGPU_VM_MUL, r, x);
                if (e0 == 4.0f) r = cw(ESPGPU_VM_MUL, r, r);
                out = {a[0].ty, r, {}};
                return true;
            }
            out = {a[0].ty, cw(ESPGPU_VM_POW, a[0].s, a[1].s), {}};
            return true;
        }
        if (name == "exp") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            out = {a[0].ty, exp_v(a[0].s), {}};
            return true;
        }
        if (name == "log") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            out = {a[0].ty, cw(ESPGPU_VM_MUL, cw(ESPGPU_VM_LOG2, a[0].s), splat(K(0.69314718f), a[0].s.size())), {}};
            return true;
        }
        if (name == "exp2") { f1(ESPGPU_VM_EXP2); return true; }
        if (name == "log2") { f1(ESPGPU_VM_LOG2); return true; }
        if (name == "sqrt") { f1(ESPGPU_VM_SQRT); return true; }
        if (name == "inversesqrt") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            if (a[0].s.size() == 1) {
                out = {a[0].ty, {unary_bcast(ESPGPU_VM_RSQ, a[0].s[0])}, {}}; /* 标量一条 RSQ */
            } else {
                out = {a[0].ty, cw(ESPGPU_VM_DIV, splat(K(1), a[0].s.size()), cw(ESPGPU_VM_SQRT, a[0].s)), {}};
            }
            return true;
        }
        if (name == "abs" || name == "sign") {
            need(na == 1 && (a[0].ty.is_scalar() || a[0].ty.is_vector()) && a[0].ty.numeric() && a[0].ty.bt != BT::UInt, l, name, a);
            out = {a[0].ty, cw(name == "abs" ? ESPGPU_VM_ABS : ESPGPU_VM_SIGN, a[0].s), {}};
            return true;
        }
        if (name == "floor") { f1(ESPGPU_VM_FLR); return true; }
        if (name == "fract") { f1(ESPGPU_VM_FRC); return true; }
        if (name == "trunc") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            out = {a[0].ty, trunc_v(a[0].s), {}};
            return true;
        }
        if (name == "round" || name == "roundEven") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            const size_t n = a[0].s.size();
            auto r = cw(ESPGPU_VM_FLR, cw(ESPGPU_VM_ADD, a[0].s, splat(K(0.5f), n)));
            if (name == "roundEven") {
                /* 恰好 .5 且 floor(x + 0.5) 是奇数时退一：r - (r - x == 0.5) * mod(r, 2) */
                auto tie = cw(ESPGPU_VM_SEQ, cw(ESPGPU_VM_SUB, r, a[0].s), splat(K(0.5f), n));
                auto odd = cw(ESPGPU_VM_MOD, r, splat(K(2.0f), n));
                r = cw(ESPGPU_VM_SUB, r, cw(ESPGPU_VM_MUL, tie, odd));
            }
            out = {a[0].ty, r, {}};
            return true;
        }
        if (name == "ceil") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            const size_t n = a[0].s.size();
            auto ng = cw(ESPGPU_VM_SUB, splat(K(0), n), a[0].s);
            out = {a[0].ty, cw(ESPGPU_VM_SUB, splat(K(0), n), cw(ESPGPU_VM_FLR, ng)), {}};
            return true;
        }
        if (name == "mod") {
            need(na == 2 && gen_float(a[0].ty) && (a[1].ty == a[0].ty || a[1].ty == T(BT::Float)), l, name, a);
            out = {a[0].ty, cw(ESPGPU_VM_MOD, a[0].s, widen(a[1], a[0].s.size())), {}};
            return true;
        }
        if (name == "min" || name == "max") {
            need(na == 2 && (a[0].ty.is_scalar() || a[0].ty.is_vector()) && a[0].ty.numeric() &&
                     (a[1].ty == a[0].ty || a[1].ty == a[0].ty.with_vec(1)), l, name, a);
            out = {a[0].ty, cw(name == "min" ? ESPGPU_VM_MIN : ESPGPU_VM_MAX, a[0].s, widen(a[1], a[0].s.size())), {}};
            return true;
        }
        if (name == "clamp") {
            need(na == 3 && (a[0].ty.is_scalar() || a[0].ty.is_vector()) && a[0].ty.numeric() &&
                     (a[1].ty == a[0].ty || a[1].ty == a[0].ty.with_vec(1)) && a[2].ty == a[1].ty, l, name, a);
            const size_t n = a[0].s.size();
            out = {a[0].ty, cw(ESPGPU_VM_CLAMP, a[0].s, widen(a[1], n), widen(a[2], n)), {}};
            return true;
        }
        if (name == "mix") {
            need(na == 3 && gen_float(a[0].ty) && a[1].ty == a[0].ty, l, name, a);
            const size_t n = a[0].s.size();
            if (a[2].ty.bt == BT::Bool) {
                need(a[2].ty == a[0].ty.with_vec(a[0].ty.vec) || (a[2].ty.vec == a[0].ty.vec), l, name, a);
                out = {a[0].ty, cw(ESPGPU_VM_SEL, a[2].s, a[1].s, a[0].s), {}};
            } else {
                need(a[2].ty == a[0].ty || a[2].ty == T(BT::Float), l, name, a);
                out = {a[0].ty, cw(ESPGPU_VM_LRP, widen(a[2], n), a[0].s, a[1].s), {}};
            }
            return true;
        }
        if (name == "step") {
            need(na == 2 && gen_float(a[1].ty) && (a[0].ty == a[1].ty || a[0].ty == T(BT::Float)), l, name, a);
            const size_t n = a[1].s.size();
            out = {a[1].ty, cw(ESPGPU_VM_STEP, a[1].s, widen(a[0], n)), {}};
            return true;
        }
        if (name == "smoothstep") {
            need(na == 3 && gen_float(a[2].ty) && (a[0].ty == a[2].ty || a[0].ty == T(BT::Float)) && a[1].ty == a[0].ty, l, name, a);
            const size_t n = a[2].s.size();
            out = {a[2].ty, cw(ESPGPU_VM_SMOOTH, widen(a[0], n), widen(a[1], n), a[2].s), {}};
            return true;
        }
        if (name == "isnan" || name == "isinf") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            out = {a[0].ty, splat(K(0), a[0].s.size()), {}};
            out.ty.bt = BT::Bool;
            return true;
        }
        if (name == "length") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            out = {T(BT::Float), {length_s(a[0].s)}, {}};
            return true;
        }
        if (name == "distance") {
            need(na == 2 && gen_float(a[0].ty) && a[0].ty == a[1].ty, l, name, a);
            out = {T(BT::Float), {length_s(cw(ESPGPU_VM_SUB, a[0].s, a[1].s))}, {}};
            return true;
        }
        if (name == "dot") {
            need(na == 2 && gen_float(a[0].ty) && a[0].ty == a[1].ty, l, name, a);
            out = {T(BT::Float), {dot(a[0].s, a[1].s)}, {}};
            return true;
        }
        if (name == "cross") {
            need(na == 2 && a[0].ty == T(BT::Float, 3) && a[1].ty == a[0].ty, l, name, a);
            const auto &x = a[0].s;
            const auto &y = a[1].s;
            auto p = cw(ESPGPU_VM_MUL, {x[1], x[2], x[0]}, {y[2], y[0], y[1]});
            auto q = cw(ESPGPU_VM_MUL, {x[2], x[0], x[1]}, {y[1], y[2], y[0]});
            out = {a[0].ty, cw(ESPGPU_VM_SUB, p, q), {}};
            return true;
        }
        if (name == "normalize") {
            need(na == 1 && gen_float(a[0].ty), l, name, a);
            if (a[0].ty.is_scalar()) {
                out = {a[0].ty, cw(ESPGPU_VM_SIGN, a[0].s), {}};
                return true;
            }
            const Scalar r = unary_bcast(ESPGPU_VM_RSQ, dot(a[0].s, a[0].s));
            out = {a[0].ty, cw(ESPGPU_VM_MUL, a[0].s, splat(r, a[0].s.size())), {}};
            return true;
        }
        if (name == "faceforward") {
            need(na == 3 && gen_float(a[0].ty) && a[1].ty == a[0].ty && a[2].ty == a[0].ty, l, name, a);
            const Scalar d = dot(a[2].s, a[1].s);
            auto neg = cw(ESPGPU_VM_SUB, splat(K(0), a[0].s.size()), a[0].s);
            out = {a[0].ty, cw(ESPGPU_VM_CMP, splat(d, a[0].s.size()), a[0].s, neg), {}};
            return true;
        }
        if (name == "reflect") {
            need(na == 2 && gen_float(a[0].ty) && a[1].ty == a[0].ty, l, name, a);
            const size_t n = a[0].s.size();
            const Scalar d2 = cw1(ESPGPU_VM_MUL, dot(a[1].s, a[0].s), K(2));
            out = {a[0].ty, cw(ESPGPU_VM_SUB, a[0].s, cw(ESPGPU_VM_MUL, a[1].s, splat(d2, n))), {}};
            return true;
        }
        if (name == "refract") {
            need(na == 3 && gen_float(a[0].ty) && a[1].ty == a[0].ty && a[2].ty == T(BT::Float), l, name, a);
            const size_t n = a[0].s.size();
            const Scalar eta = a[2].s[0];
            const Scalar d = dot(a[1].s, a[0].s);
            /* k = 1 - eta² (1 - d²) */
            const Scalar k = cw1(ESPGPU_VM_SUB, K(1), cw1(ESPGPU_VM_MUL, cw1(ESPGPU_VM_MUL, eta, eta),
                                                         cw1(ESPGPU_VM_SUB, K(1), cw1(ESPGPU_VM_MUL, d, d))));
            const Scalar m = cw1(ESPGPU_VM_MAD, eta, d, cw1(ESPGPU_VM_SQRT, cw1(ESPGPU_VM_MAX, k, K(0))));
            auto r = cw(ESPGPU_VM_SUB, cw(ESPGPU_VM_MUL, a[0].s, splat(eta, n)), cw(ESPGPU_VM_MUL, a[1].s, splat(m, n)));
            out = {a[0].ty, cw(ESPGPU_VM_CMP, splat(k, n), splat(K(0), n), r), {}};
            return true;
        }
        if (name == "matrixCompMult") {
            need(na == 2 && a[0].ty.is_matrix() && a[0].ty == a[1].ty, l, name, a);
            out = {a[0].ty, cw(ESPGPU_VM_MUL, a[0].s, a[1].s), {}};
            return true;
        }
        if (name == "outerProduct") {
            need(na == 2 && a[0].ty.is_vector() && a[1].ty.is_vector() && a[0].ty.bt == BT::Float, l, name, a);
            Val r;
            r.ty = T(BT::Float, a[0].ty.vec, a[1].ty.vec);
            for (int c = 0; c < a[1].ty.vec; c++) {
                auto cc = cw(ESPGPU_VM_MUL, a[0].s, splat(a[1].s[static_cast<size_t>(c)], a[0].s.size()));
                r.s.insert(r.s.end(), cc.begin(), cc.end());
            }
            out = r;
            return true;
        }
        if (name == "transpose") {
            need(na == 1 && a[0].ty.is_matrix(), l, name, a);
            Val r;
            r.ty = T(BT::Float, a[0].ty.cols, a[0].ty.vec);
            for (int c = 0; c < r.ty.cols; c++)
                for (int rr = 0; rr < r.ty.vec; rr++) r.s.push_back(a[0].s[static_cast<size_t>(rr * a[0].ty.vec + c)]);
            out = r;
            return true;
        }
        if (name == "determinant") {
            need(na == 1 && a[0].ty.is_matrix() && a[0].ty.cols == a[0].ty.vec && a[0].ty.cols <= 3, l, name, a);
            const auto &m = a[0].s;
            auto M = [&](int c, int r) { return m[static_cast<size_t>(c * a[0].ty.vec + r)]; };
            auto det2 = [&](Scalar x00, Scalar x01, Scalar x10, Scalar x11) {
                return cw1(ESPGPU_VM_SUB, cw1(ESPGPU_VM_MUL, x00, x11), cw1(ESPGPU_VM_MUL, x10, x01));
            };
            Scalar d;
            if (a[0].ty.cols == 2) {
                d = det2(M(0, 0), M(0, 1), M(1, 0), M(1, 1));
            } else {
                const Scalar c0 = det2(M(1, 1), M(1, 2), M(2, 1), M(2, 2));
                const Scalar c1 = det2(M(1, 0), M(1, 2), M(2, 0), M(2, 2));
                const Scalar c2 = det2(M(1, 0), M(1, 1), M(2, 0), M(2, 1));
                d = cw1(ESPGPU_VM_ADD, cw1(ESPGPU_VM_SUB, cw1(ESPGPU_VM_MUL, M(0, 0), c0), cw1(ESPGPU_VM_MUL, M(0, 1), c1)),
                        cw1(ESPGPU_VM_MUL, M(0, 2), c2));
            }
            out = {T(BT::Float), {d}, {}};
            return true;
        }
        if (name == "lessThan" || name == "lessThanEqual" || name == "greaterThan" ||
            name == "greaterThanEqual" || name == "equal" || name == "notEqual") {
            need(na == 2 && a[0].ty.is_vector() && a[0].ty == a[1].ty &&
                     (a[0].ty.numeric() || ((name == "equal" || name == "notEqual") && a[0].ty.bt == BT::Bool)), l, name, a);
            std::vector<Scalar> r;
            if (name == "lessThan") r = cw(ESPGPU_VM_SLT, a[0].s, a[1].s);
            else if (name == "lessThanEqual") r = cw(ESPGPU_VM_SGE, a[1].s, a[0].s);
            else if (name == "greaterThan") r = cw(ESPGPU_VM_SLT, a[1].s, a[0].s);
            else if (name == "greaterThanEqual") r = cw(ESPGPU_VM_SGE, a[0].s, a[1].s);
            else if (name == "equal") r = cw(ESPGPU_VM_SEQ, a[0].s, a[1].s);
            else r = cw(ESPGPU_VM_SNE, a[0].s, a[1].s);
            Ty t = a[0].ty;
            t.bt = BT::Bool;
            out = {t, r, {}};
            return true;
        }
        if (name == "any" || name == "all" || name == "not") {
            need(na == 1 && a[0].ty.is_vector() && a[0].ty.bt == BT::Bool, l, name, a);
            if (name == "not") {
                out = {a[0].ty, cw(ESPGPU_VM_SUB, splat(K(1), a[0].s.size()), a[0].s), {}};
                return true;
            }
            Scalar acc = a[0].s[0];
            for (size_t i = 1; i < a[0].s.size(); i++) {
                acc = name == "any" ? cw1(ESPGPU_VM_MAX, acc, a[0].s[i]) : pand(acc, a[0].s[i]);
            }
            out = {T(BT::Bool), {acc}, {}};
            return true;
        }
        return false;
    }

    /* ── 调用 ── */

    bool param_matches(const Function &f, const std::vector<Val> &a)
    {
        if (f.params.size() != a.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            Ty pt = resolve(f.params[i].type);
            if (f.params[i].is_array) pt.array = const_int(*f.params[i].array_size, "array size");
            if (pt != a[i].ty) return false;
        }
        return true;
    }

    Val call(const Expr &e)
    {
        /* 构造函数 */
        if (e.ctor_array || is_type(e.name)) {
            std::vector<Val> args;
            for (const auto &x : e.args) args.push_back(rval(*x));
            TypeSpec ts;
            ts.name = e.name;
            ts.loc = e.loc;
            Ty t;
            if (e.ctor_array) {
                ts.is_array = false;
                t = resolve(ts);
                if (e.ctor.array_size) t.array = const_int(*e.ctor.array_size, "array size");
                else t.array = static_cast<int>(args.size());
            } else {
                t = resolve(ts);
            }
            return construct(t, args, e.loc);
        }

        auto it = funcs_.find(e.name);
        std::vector<Val> args;
        for (const auto &x : e.args) args.push_back(rval(*x));

        if (it == funcs_.end()) {
            Val out;
            if (builtin(e.name, args, e.loc, out)) return out;
            error(e.loc, "no function named '" + e.name + "'");
        }
        const Function *fn = nullptr;
        for (const Function *f : it->second) {
            if (f->body && param_matches(*f, args)) {
                fn = f;
                break;
            }
        }
        if (!fn) {
            for (const Function *f : it->second) {
                if (param_matches(*f, args)) error(e.loc, "function '" + e.name + "' is declared but never defined");
            }
            Val out;
            if (builtin(e.name, args, e.loc, out)) return out;
            std::string sig = e.name + "(";
            for (size_t i = 0; i < args.size(); i++) sig += (i ? ", " : "") + tname(args[i].ty);
            error(e.loc, "no matching overload for " + sig + ")");
        }
        return invoke(*fn, args, &e);
    }

    bool is_type(const std::string &n) const
    {
        static const std::set<std::string> b = {
            "float", "int", "uint", "bool", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
            "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4",
            "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4"};
        return b.count(n) > 0 || struct_by_name_.count(n) > 0;
    }

    Val invoke(const Function &fn, const std::vector<Val> &args, const Expr *call_expr)
    {
        for (const Function *f : call_stack_) {
            if (f == &fn) error(fn.loc, "recursion is not allowed ('" + fn.name + "')");
        }
        if (call_stack_.size() > 32) error(fn.loc, "call depth too large");
        call_stack_.push_back(&fn);

        Frame fr;
        fr.ret = resolve(fn.ret);
        fr.retval = {fr.ret, zeros(fr.ret), {}};
        frames_.push_back(fr);

        const Scalar save_pred = pred_;
        const Scalar save_ret = ret_acc_;
        const Scalar save_brk = brk_acc_, save_cont = cont_acc_;
        const bool save_loop = in_loop_;
        ret_acc_ = K(0);
        in_loop_ = false;

        /* 函数体看不到调用者的局部变量：只保留全局作用域 */
        std::vector<std::map<std::string, VarP>> saved_scopes(scopes_.begin() + 1, scopes_.end());
        scopes_.resize(1);
        scopes_.emplace_back();

        std::vector<VarP> pvars;
        for (size_t i = 0; i < fn.params.size(); i++) {
            const Param &pm = fn.params[i];
            auto v = std::make_shared<Var>();
            v->ty = args[i].ty;
            if (pm.storage == Storage::Out) {
                v->s = zeros(v->ty);
            } else {
                v->s = args[i].s;
            }
            v->samp = args[i].samp;
            v->readonly = pm.is_const;
            v->what = "const parameter '" + pm.name + "'";
            if (!pm.name.empty()) declare(pm.name, v, pm.loc);
            pvars.push_back(v);
        }

        exec(*fn.body);

        Val result = frames_.back().retval;
        if (fr.ret.bt != BT::Void && !frames_.back().has_ret) {
            error(fn.loc, "function '" + fn.name + "' must return a value");
        }

        scopes_.resize(1);
        scopes_.insert(scopes_.end(), saved_scopes.begin(), saved_scopes.end());
        frames_.pop_back();
        pred_ = save_pred;
        ret_acc_ = save_ret;
        brk_acc_ = save_brk;
        cont_acc_ = save_cont;
        in_loop_ = save_loop;
        call_stack_.pop_back();

        /* out / inout 写回 */
        if (call_expr) {
            for (size_t i = 0; i < fn.params.size(); i++) {
                const Param &pm = fn.params[i];
                if (pm.storage != Storage::Out && pm.storage != Storage::InOut) continue;
                LVal lv;
                if (!lval(*call_expr->args[i], lv)) error(call_expr->args[i]->loc, "argument for out parameter is not an l-value");
                Val v{pvars[i]->ty, pvars[i]->s, {}};
                assign(lv, v, call_expr->args[i]->loc);
            }
        }
        return result;
    }

    /* ── 语句 ── */

    Scalar exited() { return padd(padd(ret_acc_, brk_acc_), cont_acc_); }

    bool pred_dead()
    {
        float v;
        return isK(pred_, &v) && v == 0;
    }

    void declare_local(const Stmt &s)
    {
        if (s.qual.storage == Storage::Uniform || s.qual.storage == Storage::In ||
            s.qual.storage == Storage::Out || s.qual.varying) {
            error(s.loc, "storage qualifiers are not allowed on local variables");
        }
        Ty base = resolve(s.type);
        for (const auto &d : s.decls) {
            Ty t = base;
            if (d.is_array) {
                if (t.is_array()) error(d.loc, "arrays of arrays are not supported");
                if (!d.array_size) {
                    if (!d.init) error(d.loc, "unsized array needs an initializer");
                } else {
                    t.array = const_int(*d.array_size, "array size");
                }
            }
            if (t.bt == BT::Void) error(d.loc, "variable '" + d.name + "' declared void");
            auto v = std::make_shared<Var>();
            v->ty = t;
            if (d.init) {
                Val iv = rval(*d.init);
                if (d.is_array && !d.array_size) t.array = iv.ty.array;
                v->ty = t;
                if (iv.ty != t) error(d.loc, "cannot initialize '" + tname(t) + "' with '" + tname(iv.ty) + "'");
                if (s.qual.storage == Storage::Const) {
                    for (auto x : iv.s) {
                        if (!isK(x)) error(d.loc, "const variable '" + d.name + "' needs a constant initializer");
                    }
                }
                v->s = iv.s;
                v->samp = iv.samp;
            } else {
                if (s.qual.storage == Storage::Const) error(d.loc, "const variable '" + d.name + "' needs an initializer");
                if (t.bt == BT::Sampler) error(d.loc, "local sampler variables are not supported");
                v->s = zeros(t);
            }
            v->is_const = s.qual.storage == Storage::Const;
            v->readonly = v->is_const;
            v->what = "const variable '" + d.name + "'";
            declare(d.name, v, d.loc);
        }
    }

    void exec(const Stmt &s)
    {
        switch (s.kind) {
        case Stmt::Empty: return;
        case Stmt::Block:
            scopes_.emplace_back();
            for (const auto &c : s.body) exec(*c);
            scopes_.pop_back();
            return;
        case Stmt::Decl: declare_local(s); return;
        case Stmt::ExprS: rval(*s.expr); return;
        case Stmt::If: {
            Val c = rval(*s.cond);
            if (c.ty != T(BT::Bool)) error(s.cond->loc, "if condition must be a bool, got '" + tname(c.ty) + "'");
            const Scalar p0 = pred_;
            const Scalar ex0 = exited();
            pred_ = pand(p0, c.s[0]);
            scopes_.emplace_back();
            exec(*s.then_s);
            scopes_.pop_back();
            if (s.else_s) {
                /* else 侧的谓词要扣掉 then 侧已退出的部分（它们本来就不在 else 里，无需处理） */
                pred_ = pand(p0, pnot(c.s[0]));
                scopes_.emplace_back();
                exec(*s.else_s);
                scopes_.pop_back();
            }
            pred_ = psub(p0, psub(exited(), ex0));
            return;
        }
        case Stmt::For:
        case Stmt::While:
        case Stmt::DoWhile: loop(s); return;
        case Stmt::Return: {
            if (frames_.empty()) error(s.loc, "return outside a function");
            Frame &fr = frames_.back();
            if (s.expr) {
                Val v = rval(*s.expr);
                if (v.ty != fr.ret) error(s.loc, "return type mismatch: expected '" + tname(fr.ret) + "', got '" + tname(v.ty) + "'");
                float pv;
                if (isK(pred_, &pv) && pv != 0) fr.retval = v;
                else if (!pred_dead()) fr.retval.s = cw(ESPGPU_VM_SEL, splat(pred_, v.s.size()), v.s, fr.retval.s);
                fr.has_ret = true;
            } else if (fr.ret.bt != BT::Void) {
                error(s.loc, "non-void function must return a value");
            }
            ret_acc_ = padd(ret_acc_, pred_);
            pred_ = K(0);
            return;
        }
        case Stmt::Break:
            if (!in_loop_) error(s.loc, "break outside a loop");
            brk_acc_ = padd(brk_acc_, pred_);
            pred_ = K(0);
            return;
        case Stmt::Continue:
            if (!in_loop_) error(s.loc, "continue outside a loop");
            cont_acc_ = padd(cont_acc_, pred_);
            pred_ = K(0);
            return;
        case Stmt::Discard: {
            if (stage_ != Stage::Fragment) error(s.loc, "discard is only allowed in fragment shaders");
            if (!pred_dead()) {
                Node nd;
                nd.kind = NK::Kill;
                nd.width = 1;
                nd.nargs = 1;
                nd.a[0] = operand({pred_}, 0, 1);
                dag_.kills.push_back(add_node(nd));
            }
            /* 片元已死，之后的写入无所谓；不改谓词，便于识别 alpha 测试写法 */
            return;
        }
        }
    }

    void loop(const Stmt &s)
    {
        scopes_.emplace_back();
        if (s.init) exec(*s.init);
        const int save_loop_id = cur_loop_id_;
        const Scalar save_loop_lp = cur_loop_lp_;
        cur_loop_id_ = next_loop_id_++;
        for (auto &kv : scopes_.back()) kv.second->loop_id = cur_loop_id_;

        const Scalar p_before = pred_;
        const Scalar ret0 = ret_acc_;
        const Scalar save_brk = brk_acc_, save_cont = cont_acc_;
        const bool save_loop = in_loop_;
        in_loop_ = true;
        brk_acc_ = K(0);

        Scalar lp = pred_;
        bool checked = false;
        int iter = 0;
        for (;; iter++) {
            if (iter >= kMaxUnroll) {
                error(s.loc, "loop does not have a constant trip count (unrolled " +
                                 std::to_string(kMaxUnroll) + " iterations)");
            }
            cur_loop_lp_ = lp;
            if (s.kind != Stmt::DoWhile && s.cond) {
                pred_ = lp;
                Val c = rval(*s.cond);
                if (c.ty != T(BT::Bool)) error(s.cond->loc, "loop condition must be a bool");
                lp = pand(lp, c.s[0]);
            }
            float lv;
            if (isK(lp, &lv) && lv == 0) {
                if (!checked) {
                    /* 一次都不执行的循环体也要做类型检查 */
                    pred_ = K(0);
                    cont_acc_ = K(0);
                    scopes_.emplace_back();
                    exec(*s.then_s);
                    scopes_.pop_back();
                    if (s.step) rval(*s.step);
                }
                break;
            }
            checked = true;
            pred_ = lp;
            cur_loop_lp_ = lp;
            cont_acc_ = K(0);
            const Scalar b0 = brk_acc_, r0 = ret_acc_;
            scopes_.emplace_back();
            exec(*s.then_s);
            scopes_.pop_back();
            lp = psub(psub(lp, psub(brk_acc_, b0)), psub(ret_acc_, r0));
            pred_ = lp;
            cur_loop_lp_ = lp;
            if (s.kind == Stmt::DoWhile) {
                Val c = rval(*s.cond);
                if (c.ty != T(BT::Bool)) error(s.cond->loc, "loop condition must be a bool");
                lp = pand(lp, c.s[0]);
            } else if (s.step) {
                rval(*s.step);
            }
        }
        in_loop_ = save_loop;
        brk_acc_ = save_brk;
        cont_acc_ = save_cont;
        cur_loop_id_ = save_loop_id;
        cur_loop_lp_ = save_loop_lp;
        pred_ = psub(p_before, psub(ret_acc_, ret0));
        scopes_.pop_back();
    }

    /* ── 全局 ── */

    uint32_t gl_type(const Ty &t) const
    {
        if (t.bt == BT::Sampler) return GLT_SAMPLER_2D;
        if (t.cols > 1) return t.cols == 2 ? GLT_FLOAT_MAT2 : t.cols == 3 ? GLT_FLOAT_MAT3 : GLT_FLOAT_MAT4;
        static const uint32_t f[4] = {GLT_FLOAT, GLT_FLOAT_VEC2, GLT_FLOAT_VEC3, GLT_FLOAT_VEC4};
        static const uint32_t i[4] = {GLT_INT, GLT_INT_VEC2, GLT_INT_VEC3, GLT_INT_VEC4};
        static const uint32_t u[4] = {GLT_UINT, GLT_UINT_VEC2, GLT_UINT_VEC3, GLT_UINT_VEC4};
        static const uint32_t b[4] = {GLT_BOOL, GLT_BOOL_VEC2, GLT_BOOL_VEC3, GLT_BOOL_VEC4};
        const int k = t.vec - 1;
        switch (t.bt) {
        case BT::Int: return i[k];
        case BT::UInt: return u[k];
        case BT::Bool: return b[k];
        default: return f[k];
        }
    }

    /** 把 uniform 展平成叶子，填 Var 的标量 */
    void uniform_leaves(const std::string &name, const Ty &t, Var &v, SrcLoc l)
    {
        if (t.bt == BT::Struct) {
            const int n = t.array ? t.array : 1;
            for (int e = 0; e < n; e++) {
                const std::string base = t.array ? name + "[" + std::to_string(e) + "]" : name;
                for (const auto &m : structs_[static_cast<size_t>(t.st)].members) {
                    uniform_leaves(base + "." + m.first, m.second, v, l);
                }
            }
            return;
        }
        UniformLeaf u;
        u.name = name;
        u.gl_type = gl_type(t);
        u.array_size = t.array ? t.array : 1;
        u.is_sampler = t.bt == BT::Sampler;
        u.is_int = t.bt == BT::Int || t.bt == BT::UInt || t.bt == BT::Bool;
        u.columns = t.cols;
        u.components = t.vec;
        const int leaf = static_cast<int>(sh_.uniforms.size());
        if (u.is_sampler) {
            for (int e = 0; e < u.array_size; e++) {
                dag_.samplers.push_back({leaf, e});
                v.samp.push_back(static_cast<int>(dag_.samplers.size()) - 1);
            }
        } else {
            u.local_slot = sh_.uniform_slot_count;
            for (int e = 0; e < u.array_size; e++) {
                for (int c = 0; c < t.cols; c++) {
                    Node nd;
                    nd.kind = NK::Uniform;
                    nd.width = 4;
                    nd.index = sh_.uniform_slot_count++;
                    const int id = add_node(nd);
                    for (int r = 0; r < t.vec; r++) v.s.push_back({id, static_cast<uint8_t>(r)});
                }
            }
        }
        sh_.uniforms.push_back(u);
        (void)l;
    }

    void global_decl(const Stmt &s)
    {
        if (s.decls.empty()) {
            resolve(s.type); /* 只定义结构体 */
            return;
        }
        Storage st = s.qual.storage;
        if (s.qual.varying) st = stage_ == Stage::Vertex ? Storage::Out : Storage::In;
        if (s.qual.attribute && stage_ != Stage::Vertex) error(s.loc, "'attribute' is only allowed in vertex shaders");
        Ty base = resolve(s.type);

        for (const auto &d : s.decls) {
            Ty t = base;
            if (d.is_array) {
                if (!d.array_size) error(d.loc, "unsized arrays are not supported");
                t.array = const_int(*d.array_size, "array size");
            }
            if (t.bt == BT::Void) error(d.loc, "variable '" + d.name + "' declared void");
            auto v = std::make_shared<Var>();
            v->ty = t;

            if (st == Storage::Uniform) {
                if (d.init) error(d.loc, "uniforms cannot have initializers here");
                uniform_leaves(d.name, t, *v, d.loc);
                v->readonly = true;
                v->what = "uniform '" + d.name + "'";
            } else if (st == Storage::In) {
                if (t.is_array() || t.bt == BT::Struct || t.is_matrix() || t.bt == BT::Bool || t.bt == BT::Sampler) {
                    error(d.loc, "input '" + d.name + "' must be a scalar or vector (got '" + tname(t) + "')");
                }
                InOut io;
                io.name = d.name;
                io.components = t.vec;
                io.location = s.qual.location;
                io.is_int = t.is_int();
                const int idx = static_cast<int>(sh_.inputs.size());
                sh_.inputs.push_back(io);
                Node nd;
                nd.kind = stage_ == Stage::Vertex ? NK::Attrib : NK::Varying;
                nd.width = stage_ == Stage::Vertex ? 4 : static_cast<uint8_t>(t.vec);
                nd.index = idx;
                const int id = add_node(nd);
                v->s = outs(id, t.vec);
                v->readonly = true;
                v->what = "input '" + d.name + "'";
            } else if (st == Storage::Out) {
                if (t.is_array() || t.bt == BT::Struct || t.is_matrix() || t.bt == BT::Sampler || t.bt == BT::Bool) {
                    error(d.loc, "output '" + d.name + "' must be a scalar or vector");
                }
                InOut io;
                io.name = d.name;
                io.components = t.vec;
                io.location = s.qual.location;
                io.is_int = t.is_int();
                sh_.outputs.push_back(io);
                v->s = zeros(t);
                if (stage_ == Stage::Vertex) {
                    var_outs_.push_back(v);
                } else if (!out_color_ || s.qual.location == 0) {
                    if (t.bt != BT::Float) error(d.loc, "fragment output must be a float vector");
                    out_color_ = v;
                    out_color_comps_ = t.vec;
                }
            } else {
                if (t.bt == BT::Sampler) error(d.loc, "samplers must be uniforms");
                if (d.init) {
                    Val iv = rval(*d.init);
                    if (iv.ty != t) error(d.loc, "cannot initialize '" + tname(t) + "' with '" + tname(iv.ty) + "'");
                    if (st == Storage::Const) {
                        for (auto x : iv.s) {
                            if (!isK(x)) error(d.loc, "const variable '" + d.name + "' needs a constant initializer");
                        }
                    }
                    v->s = iv.s;
                } else {
                    if (st == Storage::Const) error(d.loc, "const variable '" + d.name + "' needs an initializer");
                    v->s = zeros(t);
                }
                v->is_const = st == Storage::Const;
                v->readonly = v->is_const;
                v->what = "const variable '" + d.name + "'";
            }
            declare(d.name, v, d.loc);
        }
    }

    /**
     * 设备的 v6 是设备像素、左上原点（屏幕）；GLSL 要窗口像素、左下原点。
     * 用隐藏 uniform "@fragcoord" = (kx, ky, bx, by) 换算，xy 一条 MAD：
     *   gl_FragCoord.xy = v6.xy * (kx, ky) + (bx, by)
     * 屏幕目标 kx = 窗口宽/设备宽，ky = -窗口高/设备高，bx = 0，by = 窗口高；
     * FBO 目标 (1, 1, 0, 0)。由运行时每次绘制填写。
     */
    void make_fragcoord()
    {
        Var hidden;
        uniform_leaves("@fragcoord", T(BT::Float, 4), hidden, {0, 0});
        Node nd;
        nd.kind = NK::FragCoord;
        nd.width = 4;
        const int id = add_node(nd);
        const auto &u = hidden.s;
        auto fc = std::make_shared<Var>();
        fc->ty = T(BT::Float, 4);
        const auto xy = cw(ESPGPU_VM_MAD, {{id, 0}, {id, 1}}, {u[0], u[1]}, {u[2], u[3]});
        fc->s = {xy[0], xy[1], {id, 2}, {id, 3}};
        fc->readonly = true;
        fc->what = "gl_FragCoord";
        scopes_[0]["gl_FragCoord"] = fc;
    }

    VarP builtin_var(const std::string &name, Ty t, bool readonly)
    {
        auto v = std::make_shared<Var>();
        v->ty = t;
        v->s = zeros(t);
        v->readonly = readonly;
        v->what = "built-in '" + name + "'";
        scopes_[0][name] = v;
        return v;
    }

    void run_inner()
    {
        scopes_.emplace_back();
        pred_ = K(1);
        ret_acc_ = brk_acc_ = cont_acc_ = K(0);

        if (stage_ == Stage::Vertex) {
            gl_position_ = builtin_var("gl_Position", T(BT::Float, 4), false);
            builtin_var("gl_PointSize", T(BT::Float), false);
            auto vid = builtin_var("gl_VertexID", T(BT::Int), true);
            (void)vid;
            builtin_var("gl_InstanceID", T(BT::Int), true);
        } else {
            /* gl_FragCoord 用到时才建（见 lval），因为它要占一个隐藏 uniform 槽 */
            auto ff = builtin_var("gl_FrontFacing", T(BT::Bool), true);
            ff->s = {K(1)};
            auto pc = builtin_var("gl_PointCoord", T(BT::Float, 2), true);
            pc->s = {K(0.5f), K(0.5f)};
            if (version_ < 300) {
                auto fcol = builtin_var("gl_FragColor", T(BT::Float, 4), false);
                Ty fd = T(BT::Float, 4);
                fd.array = 1;
                auto fdata = builtin_var("gl_FragData", fd, false);
                (void)fcol;
                (void)fdata;
            }
        }
        /* 常用的 gl_Max* 常量 */
        auto mx = builtin_var("gl_MaxVertexAttribs", T(BT::Int), true);
        mx->s = {K(8)};
        auto mt = builtin_var("gl_MaxTextureImageUnits", T(BT::Int), true);
        mt->s = {K(4)};
        auto md = builtin_var("gl_MaxDrawBuffers", T(BT::Int), true);
        md->s = {K(1)};

        /* 先收集函数（允许先用后定义） */
        for (const auto &g : tu_.globals) {
            if (g.kind == Global::Func) funcs_[g.func->name].push_back(g.func.get());
        }

        const Function *main_fn = nullptr;
        for (const auto &g : tu_.globals) {
            if (g.kind == Global::Var) {
                global_decl(*g.decl);
            } else if (g.kind == Global::Func && g.func->name == "main" && g.func->body) {
                if (!g.func->params.empty()) error(g.func->loc, "main() must not take parameters");
                main_fn = g.func.get();
            }
        }
        if (!main_fn) error({1, 1}, "missing main() function");

        if (stage_ == Stage::Fragment && version_ < 300) {
            if (out_color_) error({1, 1}, "'out' variables require #version 300 es");
        }

        invoke(*main_fn, {}, nullptr);

        /* 没有被调用的函数空跑一遍做类型检查 */
        std::set<const Function *> checked;
        for (const auto &g : tu_.globals) {
            if (g.kind != Global::Func || !g.func->body || g.func.get() == main_fn) continue;
            const Function &f = *g.func;
            std::vector<Val> args;
            for (const auto &pm : f.params) {
                Val a;
                a.ty = resolve(pm.type);
                if (pm.is_array) a.ty.array = const_int(*pm.array_size, "array size");
                Node nd;
                nd.kind = NK::Varying;
                nd.index = -1;
                nd.width = 4;
                nd.c = static_cast<float>(dag_.nodes.size()); /* 唯一化，避免被 CSE 合并后被当常量 */
                const int n = count(a.ty);
                for (int i = 0; i < n; i += 4) {
                    nd.c += 1;
                    const int id = add_node(nd);
                    for (int k = 0; k < 4 && i + k < n; k++) a.s.push_back({id, static_cast<uint8_t>(k)});
                }
                for (int i = 0; i < samp_count(a.ty); i++) a.samp.push_back(-1);
                args.push_back(a);
            }
            const Scalar save = pred_;
            pred_ = K(0);
            invoke(f, args, nullptr);
            pred_ = save;
        }

        /* 输出 */
        if (stage_ == Stage::Fragment) {
            std::vector<Scalar> c;
            if (out_color_) {
                c = out_color_->s;
            } else if (version_ < 300) {
                VarP fc = scopes_[0]["gl_FragColor"];
                VarP fd = scopes_[0]["gl_FragData"];
                /* 两者都写时取 gl_FragColor：用"是否仍是全零常量"判断哪个被写过 */
                bool fc_written = false;
                for (auto s : fc->s) fc_written = fc_written || !isK(s, 0.0f);
                c = fc_written ? fc->s : fd->s;
            }
            if (!c.empty()) {
                while (c.size() < 3) c.push_back(K(0));
                if (c.size() < 4) c.push_back(K(1));
                dag_.has_color = true;
                dag_.color = operand(c, 0, 4);
            }
        } else {
            dag_.position = operand(gl_position_->s, 0, 4);
            for (const auto &v : var_outs_) dag_.var_out.push_back(operand(v->s, 0, v->s.size()));
        }
    }

public:
    bool run_safe()
    {
        try {
            run_inner();
            return true;
        } catch (const CompileError &e) {
            sh_.log += std::to_string(e.loc.line) + ":" + std::to_string(e.loc.col) + ": error: " + e.msg + "\n";
            return false;
        }
    }
};

bool Lower::run()
{
    return run_safe();
}

} // namespace

bool lower(const TranslationUnit &tu, Stage stage, int version, Shader &sh)
{
    if (!sh.dag) sh.dag = std::make_shared<Dag>();
    Lower lw(tu, stage, version, sh);
    return lw.run();
}

} // namespace espgpu::glsl
