/*
 * compiler.cpp — 编译驱动、顶点着色器 T&L 识别、链接
 */

#include "internal.h"

#include <algorithm>
#include <functional>
#include <set>

namespace espgpu::glsl {

/* ── 顶点着色器 T&L 识别（在 AST 上做，比在 DAG 上匹配矩阵链简单得多） ── */

namespace {

const Function *find_main(const TranslationUnit &tu)
{
    for (const auto &g : tu.globals) {
        if (g.kind == Global::Func && g.func->name == "main" && g.func->body) return g.func.get();
    }
    return nullptr;
}

int find_input(const Shader &sh, const std::string &n)
{
    for (size_t i = 0; i < sh.inputs.size(); i++) {
        if (sh.inputs[i].name == n) return static_cast<int>(i);
    }
    return -1;
}

int find_output(const Shader &sh, const std::string &n)
{
    for (size_t i = 0; i < sh.outputs.size(); i++) {
        if (sh.outputs[i].name == n) return static_cast<int>(i);
    }
    return -1;
}

int find_mat4_uniform(const Shader &sh, const std::string &n)
{
    for (size_t i = 0; i < sh.uniforms.size(); i++) {
        const auto &u = sh.uniforms[i];
        if (u.name == n && u.gl_type == GLT_FLOAT_MAT4 && u.array_size == 1) return static_cast<int>(i);
    }
    return -1;
}

struct VsMatch {
    const Shader &sh;
    std::map<std::string, const Expr *> locals;

    const Expr *subst(const Expr *e) const
    {
        while (e && e->kind == Expr::Ident) {
            auto it = locals.find(e->name);
            if (it == locals.end()) break;
            e = it->second;
        }
        return e;
    }

    /** 属性引用：a 或 a.x / a.xy / a.xyz / a.xyzw（必须从 x 开始的恒等前缀） */
    bool attrib_ref(const Expr *e, int &input, int &comps) const
    {
        e = subst(e);
        if (!e) return false;
        if (e->kind == Expr::Ident) {
            input = find_input(sh, e->name);
            if (input < 0) return false;
            comps = sh.inputs[static_cast<size_t>(input)].components;
            return !sh.inputs[static_cast<size_t>(input)].is_int;
        }
        if (e->kind == Expr::Field && e->args[0]->kind == Expr::Ident) {
            static const char *pre[] = {"x", "xy", "xyz", "xyzw", "r", "rg", "rgb", "rgba"};
            int n = -1;
            for (int i = 0; i < 8; i++) {
                if (e->name == pre[i]) n = (i % 4) + 1;
            }
            if (n < 0) return false;
            input = find_input(sh, e->args[0]->name);
            if (input < 0 || sh.inputs[static_cast<size_t>(input)].is_int) return false;
            if (n > sh.inputs[static_cast<size_t>(input)].components) return false;
            comps = n;
            return true;
        }
        return false;
    }

    static bool is_float_lit(const Expr *e, double v)
    {
        return e && (e->kind == Expr::FloatLit || e->kind == Expr::IntLit) && e->num == v;
    }

    /** 位置向量：vec4 属性，或 vec4(a.xyz, 1.0)，或 vec4(a.xy, 0.0, 1.0) */
    bool position(const Expr *e, int &input, int &comps) const
    {
        e = subst(e);
        if (!e) return false;
        int in, n;
        if (attrib_ref(e, in, n)) {
            if (n != 4) return false;
            input = in;
            comps = 4;
            return true;
        }
        if (e->kind != Expr::Call || e->name != "vec4") return false;
        if (e->args.empty() || !attrib_ref(e->args[0].get(), in, n)) return false;
        /* 设备补齐规则：2 分量 → z=0 w=1，3 分量 → w=1 */
        if (n == 3 && e->args.size() == 2 && is_float_lit(e->args[1].get(), 1.0)) {
            input = in;
            comps = 3;
            return true;
        }
        if (n == 2 && e->args.size() == 3 && is_float_lit(e->args[1].get(), 0.0) &&
            is_float_lit(e->args[2].get(), 1.0)) {
            input = in;
            comps = 2;
            return true;
        }
        if (n == 4 && e->args.size() == 1) {
            input = in;
            comps = 4;
            return true;
        }
        return false;
    }

    bool product(const Expr *e, std::vector<int> &mats, int &input, int &comps) const
    {
        e = subst(e);
        if (!e) return false;
        if (e->kind == Expr::Binary && e->op == "*") {
            /* 右侧是位置，左侧是矩阵链 */
            std::function<bool(const Expr *)> chain = [&](const Expr *m) {
                m = subst(m);
                if (!m) return false;
                if (m->kind == Expr::Binary && m->op == "*") return chain(m->args[0].get()) && chain(m->args[1].get());
                if (m->kind != Expr::Ident) return false;
                const int u = find_mat4_uniform(sh, m->name);
                if (u < 0) return false;
                mats.push_back(u);
                return true;
            };
            return chain(e->args[0].get()) && position(e->args[1].get(), input, comps);
        }
        return false;
    }
};

} // namespace

void recognize_vertex(const TranslationUnit &tu, Shader &sh)
{
    VertexPlan vp;
    vp.varying_input.assign(sh.outputs.size(), -1);
    const Function *m = find_main(tu);
    if (!m) return;

    VsMatch vm{sh, {}};
    std::set<std::string> assigned_locals;
    bool have_pos = false;

    for (const auto &st : m->body->body) {
        if (st->kind == Stmt::Empty) continue;
        if (st->kind == Stmt::Decl) {
            for (const auto &d : st->decls) {
                if (d.is_array || !d.init) return;
                vm.locals[d.name] = d.init.get();
            }
            continue;
        }
        if (st->kind != Stmt::ExprS || st->expr->kind != Expr::Assign || st->expr->op != "=") return;
        const Expr &lhs = *st->expr->args[0];
        const Expr *rhs = st->expr->args[1].get();
        if (lhs.kind != Expr::Ident) return;
        if (vm.locals.count(lhs.name)) return; /* 局部变量被重新赋值：放弃 */
        if (lhs.name == "gl_Position") {
            if (have_pos) return;
            vp.mvp_uniforms.clear();
            if (!vm.product(rhs, vp.mvp_uniforms, vp.pos_input, vp.pos_components)) {
                /* 没有矩阵也可以：gl_Position = 属性（已是裁剪坐标） */
                if (!vm.position(rhs, vp.pos_input, vp.pos_components)) return;
            }
            have_pos = true;
        } else if (lhs.name == "gl_PointSize") {
            continue;
        } else {
            const int o = find_output(sh, lhs.name);
            if (o < 0 || vp.varying_input[static_cast<size_t>(o)] >= 0) return;
            int in, n;
            if (!vm.attrib_ref(rhs, in, n) || n != sh.outputs[static_cast<size_t>(o)].components) return;
            vp.varying_input[static_cast<size_t>(o)] = in;
        }
    }
    if (!have_pos) return;
    vp.tnl = true;
    sh.vert = vp;
}

/* ── 驱动 ─────────────────────────────────────────────────────────── */

namespace {

void vertex_live(Shader &sh)
{
    Dag &d = *sh.dag;
    std::vector<char> seen(d.nodes.size(), 0);
    std::function<void(int)> mark = [&](int id) {
        if (id < 0 || seen[static_cast<size_t>(id)]) return;
        seen[static_cast<size_t>(id)] = 1;
        const Node &n = d.nodes[static_cast<size_t>(id)];
        for (int k = 0; k < n.nargs; k++)
            for (int i = 0; i < n.a[k].n; i++) mark(n.a[k].s[i].node);
    };
    for (int i = 0; i < d.position.n; i++) mark(d.position.s[i].node);
    for (const auto &o : d.var_out)
        for (int i = 0; i < o.n; i++) mark(o.s[i].node);
    d.live_order.clear();
    d.pre_order.clear();
    /* 只依赖 uniform / 常量的节点（uo）整批只算一次，其余逐顶点 */
    std::vector<char> uo(d.nodes.size(), 0);
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
    for (size_t id = 0; id < d.nodes.size(); id++) {
        if (!seen[id]) continue;
        (uo[id] ? d.pre_order : d.live_order).push_back(static_cast<int>(id));
        const Node &n = d.nodes[id];
        if (n.kind == NK::Attrib && n.index >= 0) sh.inputs[static_cast<size_t>(n.index)].used = true;
        if (n.kind == NK::Uniform) {
            for (auto &u : sh.uniforms) {
                if (!u.is_sampler && n.index >= u.local_slot && n.index < u.local_slot + u.array_size * u.columns) u.used = true;
            }
        }
    }
    for (auto &o : sh.outputs) o.used = true;
}

} // namespace

Shader compile(Stage stage, const std::string &source)
{
    Shader sh;
    sh.stage = stage;
    sh.dag = std::make_shared<Dag>();

    std::string pp, err;
    int version = 100;
    if (!preprocess(source, pp, err, &version)) {
        sh.log = err + "\n";
        return sh;
    }
    sh.version = version;

    std::vector<Token> toks;
    if (!lex(pp, toks, err)) {
        sh.log = err + "\n";
        return sh;
    }
    TranslationUnit tu;
    if (!parse(toks, tu, err)) {
        sh.log = err + "\n";
        return sh;
    }
    if (!lower(tu, stage, version, sh)) return sh;

    if (stage == Stage::Fragment) {
        if (!codegen_fragment(sh)) return sh;
    } else {
        vertex_live(sh);
        recognize_vertex(tu, sh);
    }
    sh.ok = true;
    return sh;
}

/* ── 链接 ─────────────────────────────────────────────────────────── */

Program link(const Shader &vs, const Shader &fs)
{
    Program p;
    if (!vs.ok || !fs.ok) {
        p.log = "cannot link: shader not compiled\n";
        return p;
    }
    if (vs.stage != Stage::Vertex || fs.stage != Stage::Fragment) {
        p.log = "cannot link: need one vertex and one fragment shader\n";
        return p;
    }
    if (vs.version != fs.version && !(vs.version >= 300 && fs.version >= 300)) {
        p.log = "shader versions do not match (" + std::to_string(vs.version) + " vs " + std::to_string(fs.version) + ")\n";
        return p;
    }

    auto add = [&](const UniformLeaf &u, bool is_vs, int leaf) -> bool {
        for (auto &pu : p.uniforms) {
            if (pu.name != u.name) continue;
            if (pu.gl_type != u.gl_type || pu.array_size != u.array_size) {
                p.log += "uniform '" + u.name + "' has different types in the vertex and fragment shader\n";
                return false;
            }
            (is_vs ? pu.vs_leaf : pu.fs_leaf) = leaf;
            return true;
        }
        ProgramUniform pu;
        pu.name = u.name;
        pu.gl_type = u.gl_type;
        pu.array_size = u.array_size;
        pu.columns = u.columns;
        pu.components = u.components;
        pu.is_sampler = u.is_sampler;
        pu.is_int = u.is_int;
        pu.storage = p.storage_slots;
        p.storage_slots += u.array_size * (u.is_sampler ? 1 : u.columns);
        (is_vs ? pu.vs_leaf : pu.fs_leaf) = leaf;
        p.uniforms.push_back(pu);
        return true;
    };
    for (size_t i = 0; i < vs.uniforms.size(); i++) {
        if (!add(vs.uniforms[i], true, static_cast<int>(i))) return p;
    }
    for (size_t i = 0; i < fs.uniforms.size(); i++) {
        if (!add(fs.uniforms[i], false, static_cast<int>(i))) return p;
    }

    /* 编译器内部的隐藏 uniform（名字以 '@' 开头）排到最后，运行时不对外暴露 */
    std::stable_partition(p.uniforms.begin(), p.uniforms.end(),
                          [](const ProgramUniform &u) { return u.name.empty() || u.name[0] != '@'; });
    p.visible_uniforms = 0;
    for (const auto &u : p.uniforms) p.visible_uniforms += (u.name.empty() || u.name[0] != '@') ? 1 : 0;

    p.vs_out_for_fs_in.assign(fs.inputs.size(), -1);
    for (size_t j = 0; j < fs.inputs.size(); j++) {
        const InOut &in = fs.inputs[j];
        int found = -1;
        for (size_t i = 0; i < vs.outputs.size(); i++) {
            if (vs.outputs[i].name == in.name) found = static_cast<int>(i);
        }
        if (found < 0) {
            if (in.used) {
                p.log += "fragment input '" + in.name + "' is not written by the vertex shader\n";
                return p;
            }
            continue;
        }
        if (vs.outputs[static_cast<size_t>(found)].components != in.components ||
            vs.outputs[static_cast<size_t>(found)].is_int != in.is_int) {
            p.log += "varying '" + in.name + "' has different types in the two stages\n";
            return p;
        }
        p.vs_out_for_fs_in[j] = found;
    }
    p.ok = true;
    return p;
}

} // namespace espgpu::glsl
