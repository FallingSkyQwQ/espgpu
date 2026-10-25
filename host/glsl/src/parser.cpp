/*
 * parser.cpp — 词法分析 + 递归下降语法分析
 */

#include "internal.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <set>

namespace espgpu::glsl {

namespace {

const std::set<std::string> &keywords()
{
    static const std::set<std::string> k = {
        "attribute", "const", "uniform", "varying", "layout", "centroid", "flat", "smooth",
        "break", "continue", "do", "for", "while", "switch", "case", "default", "if", "else",
        "in", "out", "inout", "invariant", "discard", "return", "lowp", "mediump", "highp",
        "precision", "struct", "true", "false",
    };
    return k;
}

std::string loc_str(SrcLoc l)
{
    return std::to_string(l.line) + ":" + std::to_string(l.col);
}

} // namespace

/* ── 词法 ─────────────────────────────────────────────────────────── */

bool lex(const std::string &src, std::vector<Token> &out, std::string &err)
{
    out.clear();
    int line = 1;
    int col = 1;
    size_t i = 0;
    auto adv = [&](size_t n) {
        for (size_t k = 0; k < n && i < src.size(); k++, i++) {
            if (src[i] == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
        }
    };

    while (i < src.size()) {
        const char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            adv(1);
            continue;
        }
        Token t;
        t.loc = {line, col};
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < src.size() &&
                   (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_')) {
                j++;
            }
            t.text = src.substr(i, j - i);
            if (t.text == "true" || t.text == "false") {
                t.kind = Tok::Bool;
                t.num = t.text == "true" ? 1 : 0;
            } else {
                t.kind = keywords().count(t.text) ? Tok::Keyword : Tok::Ident;
            }
            adv(j - i);
            out.push_back(t);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            size_t j = i;
            bool is_float = false;
            bool hex = false;
            if (c == '0' && j + 1 < src.size() && (src[j + 1] == 'x' || src[j + 1] == 'X')) {
                hex = true;
                j += 2;
                while (j < src.size() && std::isxdigit(static_cast<unsigned char>(src[j]))) j++;
            } else {
                while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) j++;
                if (j < src.size() && src[j] == '.') {
                    is_float = true;
                    j++;
                    while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) j++;
                }
                if (j < src.size() && (src[j] == 'e' || src[j] == 'E')) {
                    size_t k = j + 1;
                    if (k < src.size() && (src[k] == '+' || src[k] == '-')) k++;
                    if (k < src.size() && std::isdigit(static_cast<unsigned char>(src[k]))) {
                        is_float = true;
                        j = k;
                        while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) j++;
                    }
                }
            }
            const std::string body = src.substr(i, j - i);
            bool is_uint = false;
            if (j < src.size() && (src[j] == 'f' || src[j] == 'F') && !hex) {
                is_float = true;
                j++;
            } else if (j < src.size() && (src[j] == 'u' || src[j] == 'U')) {
                is_uint = true;
                j++;
            }
            if (j < src.size() && (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_')) {
                err = loc_str(t.loc) + ": error: invalid numeric literal '" +
                      src.substr(i, j + 1 - i) + "'";
                return false;
            }
            t.text = src.substr(i, j - i);
            if (is_float) {
                t.kind = Tok::Float;
                t.num = std::strtod(body.c_str(), nullptr);
            } else {
                t.kind = is_uint ? Tok::UInt : Tok::Int;
                const bool oct = !hex && body.size() > 1 && body[0] == '0';
                errno = 0;
                const unsigned long long v = std::strtoull(body.c_str(), nullptr, hex ? 16 : (oct ? 8 : 10));
                if (errno == ERANGE || v > 0xFFFFFFFFull ||
                    (oct && body.find_first_of("89") != std::string::npos)) {
                    err = loc_str(t.loc) + ": error: invalid integer literal '" + t.text + "'";
                    return false;
                }
                t.num = static_cast<double>(v);
            }
            adv(j - i);
            out.push_back(t);
            continue;
        }
        static const char *ops[] = {"<<=", ">>=", "++", "--", "<=", ">=", "==", "!=", "&&", "||",
                                    "^^", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<",
                                    ">>"};
        bool m = false;
        for (const char *op : ops) {
            const size_t n = std::char_traits<char>::length(op);
            if (src.compare(i, n, op) == 0) {
                t.kind = Tok::Punct;
                t.text = op;
                adv(n);
                m = true;
                break;
            }
        }
        if (!m) {
            if (std::string("+-*/%<>=!&|^~?:;,.()[]{}").find(c) == std::string::npos) {
                err = loc_str(t.loc) + ": error: unexpected character '" + std::string(1, c) + "'";
                return false;
            }
            t.kind = Tok::Punct;
            t.text = std::string(1, c);
            adv(1);
        }
        out.push_back(t);
    }
    Token e;
    e.kind = Tok::End;
    e.loc = {line, col};
    out.push_back(e);
    return true;
}

/* ── 语法 ─────────────────────────────────────────────────────────── */

namespace {

bool is_builtin_type(const std::string &s)
{
    static const std::set<std::string> t = {
        "void", "bool", "int", "uint", "float", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
        "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4", "mat2x2",
        "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4",
        "sampler2D", "sampler3D", "samplerCube", "sampler2DShadow", "samplerCubeShadow",
        "sampler2DArray", "sampler2DArrayShadow", "isampler2D", "isampler3D", "isamplerCube",
        "isampler2DArray", "usampler2D", "usampler3D", "usamplerCube", "usampler2DArray",
        "samplerExternalOES",
    };
    return t.count(s) > 0;
}

struct Parser {
    const std::vector<Token> &t;
    TranslationUnit &tu;
    size_t p = 0;
    std::string err;
    std::set<std::string> struct_names;

    Parser(const std::vector<Token> &toks, TranslationUnit &u) : t(toks), tu(u) {}

    const Token &cur() const { return t[p]; }
    const Token &at(size_t k) const { return t[std::min(p + k, t.size() - 1)]; }

    bool fail(const std::string &m, SrcLoc l)
    {
        if (err.empty()) {
            err = loc_str(l) + ": error: " + m;
        }
        return false;
    }
    bool fail(const std::string &m) { return fail(m, cur().loc); }

    bool is(const char *s) const
    {
        return (cur().kind == Tok::Punct || cur().kind == Tok::Keyword) && cur().text == s;
    }
    bool accept(const char *s)
    {
        if (is(s)) {
            p++;
            return true;
        }
        return false;
    }
    bool expect(const char *s)
    {
        if (accept(s)) return true;
        const std::string got = cur().kind == Tok::End ? "end of input" : "'" + cur().text + "'";
        return fail(std::string("expected '") + s + "' but found " + got);
    }

    bool is_type_name(const Token &k) const
    {
        return k.kind == Tok::Ident && (is_builtin_type(k.text) || struct_names.count(k.text));
    }

    bool skip_precision()
    {
        while (cur().kind == Tok::Keyword &&
               (cur().text == "lowp" || cur().text == "mediump" || cur().text == "highp")) {
            p++;
        }
        return true;
    }

    /* 限定符：返回是否读到了任何限定符 */
    bool qualifiers(Qualifier &q, bool &any)
    {
        any = false;
        for (;;) {
            if (cur().kind != Tok::Keyword) break;
            const std::string &s = cur().text;
            if (s == "const") {
                q.storage = Storage::Const;
            } else if (s == "uniform") {
                q.storage = Storage::Uniform;
            } else if (s == "in" || s == "attribute") {
                q.storage = Storage::In;
                q.attribute = s == "attribute";
            } else if (s == "varying") {
                q.storage = Storage::None; /* 由 lower 按阶段解释 */
                q.varying = true;
            } else if (s == "out") {
                q.storage = Storage::Out;
            } else if (s == "inout") {
                q.storage = Storage::InOut;
            } else if (s == "invariant") {
                q.invariant = true;
            } else if (s == "centroid" || s == "flat" || s == "smooth") {
                /* 忽略 */
            } else if (s == "lowp" || s == "mediump" || s == "highp") {
                p++; /* 精度不算存储限定符（函数返回类型也能带精度） */
                continue;
            } else if (s == "layout") {
                p++;
                if (!expect("(")) return false;
                for (;;) {
                    if (cur().kind != Tok::Ident) return fail("expected layout qualifier name");
                    const std::string name = cur().text;
                    p++;
                    if (accept("=")) {
                        if (cur().kind != Tok::Int && cur().kind != Tok::UInt) {
                            return fail("expected integer in layout qualifier");
                        }
                        if (name == "location") q.location = static_cast<int>(cur().num);
                        p++;
                    }
                    if (accept(",")) continue;
                    if (!expect(")")) return false;
                    break;
                }
                any = true;
                continue;
            } else {
                break;
            }
            any = true;
            p++;
        }
        return true;
    }

    bool type_spec(TypeSpec &ts)
    {
        skip_precision();
        ts.loc = cur().loc;
        if (accept("struct")) {
            auto sd = std::make_unique<StructDecl>();
            sd->loc = ts.loc;
            if (cur().kind == Tok::Ident) {
                sd->name = cur().text;
                p++;
            } else {
                sd->name = "__anon" + std::to_string(tu.structs.size());
            }
            struct_names.insert(sd->name);
            if (!expect("{")) return false;
            while (!is("}")) {
                if (cur().kind == Tok::End) return fail("unterminated struct");
                TypeSpec mt;
                Qualifier mq;
                bool any;
                if (!qualifiers(mq, any)) return false;
                if (!type_spec(mt)) return false;
                for (;;) {
                    StructMember m;
                    m.type = mt;
                    m.loc = cur().loc;
                    if (cur().kind != Tok::Ident) return fail("expected member name");
                    m.name = cur().text;
                    p++;
                    if (accept("[")) {
                        m.is_array = true;
                        if (!is("]")) {
                            m.array_size = expr();
                            if (!m.array_size) return false;
                        }
                        if (!expect("]")) return false;
                    }
                    sd->members.push_back(std::move(m));
                    if (accept(",")) continue;
                    break;
                }
                if (!expect(";")) return false;
            }
            p++;
            ts.name = sd->name;
            ts.inline_struct = sd.get();
            tu.structs.push_back(std::move(sd));
        } else {
            if (!is_type_name(cur())) {
                return fail("expected a type name but found '" + cur().text + "'");
            }
            ts.name = cur().text;
            p++;
        }
        if (accept("[")) {
            ts.is_array = true;
            if (!is("]")) {
                ExprP e = expr();
                if (!e) return false;
                ts.array_size = e.get();
                tu.owned_exprs.push_back(std::move(e));
            }
            if (!expect("]")) return false;
        }
        return true;
    }

    /* ── 表达式 ── */

    ExprP make(Expr::Kind k, SrcLoc l)
    {
        auto e = std::make_unique<Expr>();
        e->kind = k;
        e->loc = l;
        return e;
    }

    ExprP expr()
    {
        ExprP e = assign();
        while (e && is(",")) {
            auto c = make(Expr::Comma, cur().loc);
            p++;
            ExprP r = assign();
            if (!r) return nullptr;
            c->args.push_back(std::move(e));
            c->args.push_back(std::move(r));
            e = std::move(c);
        }
        return e;
    }

    ExprP assign()
    {
        ExprP lhs = ternary();
        if (!lhs) return nullptr;
        static const char *aops[] = {"=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=",
                                     "|=", "^="};
        for (const char *op : aops) {
            if (cur().kind == Tok::Punct && cur().text == op) {
                auto a = make(Expr::Assign, cur().loc);
                a->op = op;
                p++;
                ExprP rhs = assign();
                if (!rhs) return nullptr;
                a->args.push_back(std::move(lhs));
                a->args.push_back(std::move(rhs));
                return a;
            }
        }
        return lhs;
    }

    ExprP ternary()
    {
        ExprP c = binary(0);
        if (!c) return nullptr;
        if (is("?")) {
            auto e = make(Expr::Ternary, cur().loc);
            p++;
            ExprP a = expr();
            if (!a || !expect(":")) return nullptr;
            ExprP b = assign();
            if (!b) return nullptr;
            e->args.push_back(std::move(c));
            e->args.push_back(std::move(a));
            e->args.push_back(std::move(b));
            return e;
        }
        return c;
    }

    static int prec(const std::string &op)
    {
        if (op == "||") return 1;
        if (op == "^^") return 2;
        if (op == "&&") return 3;
        if (op == "|") return 4;
        if (op == "^") return 5;
        if (op == "&") return 6;
        if (op == "==" || op == "!=") return 7;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 8;
        if (op == "<<" || op == ">>") return 9;
        if (op == "+" || op == "-") return 10;
        if (op == "*" || op == "/" || op == "%") return 11;
        return -1;
    }

    ExprP binary(int min_prec)
    {
        ExprP lhs = unary();
        while (lhs && cur().kind == Tok::Punct) {
            const int pr = prec(cur().text);
            if (pr < 0 || pr < min_prec) break;
            auto b = make(Expr::Binary, cur().loc);
            b->op = cur().text;
            p++;
            ExprP rhs = binary(pr + 1);
            if (!rhs) return nullptr;
            b->args.push_back(std::move(lhs));
            b->args.push_back(std::move(rhs));
            lhs = std::move(b);
        }
        return lhs;
    }

    ExprP unary()
    {
        if (cur().kind == Tok::Punct) {
            const std::string op = cur().text;
            if (op == "+" || op == "-" || op == "!" || op == "~") {
                auto u = make(Expr::Unary, cur().loc);
                u->op = op;
                p++;
                ExprP a = unary();
                if (!a) return nullptr;
                u->args.push_back(std::move(a));
                return u;
            }
            if (op == "++" || op == "--") {
                auto u = make(Expr::PreInc, cur().loc);
                u->op = op;
                p++;
                ExprP a = unary();
                if (!a) return nullptr;
                u->args.push_back(std::move(a));
                return u;
            }
        }
        return postfix();
    }

    ExprP postfix()
    {
        ExprP e = primary();
        while (e) {
            if (is("[")) {
                auto x = make(Expr::Index, cur().loc);
                p++;
                ExprP i = expr();
                if (!i || !expect("]")) return nullptr;
                x->args.push_back(std::move(e));
                x->args.push_back(std::move(i));
                e = std::move(x);
            } else if (is(".")) {
                auto f = make(Expr::Field, cur().loc);
                p++;
                if (cur().kind != Tok::Ident) {
                    fail("expected field name after '.'");
                    return nullptr;
                }
                f->name = cur().text;
                p++;
                if (is("(")) {
                    /* a.length() */
                    p++;
                    if (!expect(")")) return nullptr;
                    f->name += "()";
                }
                f->args.push_back(std::move(e));
                e = std::move(f);
            } else if (is("++") || is("--")) {
                auto u = make(Expr::PostInc, cur().loc);
                u->op = cur().text;
                p++;
                u->args.push_back(std::move(e));
                e = std::move(u);
            } else {
                break;
            }
        }
        return e;
    }

    bool call_args(Expr &c)
    {
        if (!expect("(")) return false;
        if (accept(")")) return true;
        if (cur().kind == Tok::Ident && cur().text == "void" && at(1).text == ")") {
            p += 2;
            return true;
        }
        for (;;) {
            ExprP a = assign();
            if (!a) return false;
            c.args.push_back(std::move(a));
            if (accept(",")) continue;
            return expect(")");
        }
    }

    ExprP primary()
    {
        const Token &k = cur();
        switch (k.kind) {
        case Tok::Int:
        case Tok::UInt:
        case Tok::Float:
        case Tok::Bool: {
            auto e = make(k.kind == Tok::Int     ? Expr::IntLit
                          : k.kind == Tok::UInt  ? Expr::UIntLit
                          : k.kind == Tok::Float ? Expr::FloatLit
                                                 : Expr::BoolLit,
                          k.loc);
            e->num = k.num;
            p++;
            return e;
        }
        case Tok::Ident: {
            if (is_type_name(k)) {
                /* 构造函数：T(...) 或 T[N](...) */
                auto e = make(Expr::Call, k.loc);
                e->name = k.text;
                p++;
                if (is("[")) {
                    p++;
                    e->ctor_array = true;
                    e->ctor.name = k.text;
                    e->ctor.is_array = true;
                    if (!is("]")) {
                        ExprP sz = expr();
                        if (!sz) return nullptr;
                        e->ctor.array_size = sz.get();
                        tu.owned_exprs.push_back(std::move(sz));
                    }
                    if (!expect("]")) return nullptr;
                }
                if (!call_args(*e)) return nullptr;
                return e;
            }
            if (at(1).kind == Tok::Punct && at(1).text == "(") {
                auto e = make(Expr::Call, k.loc);
                e->name = k.text;
                p++;
                if (!call_args(*e)) return nullptr;
                return e;
            }
            auto e = make(Expr::Ident, k.loc);
            e->name = k.text;
            p++;
            return e;
        }
        case Tok::Punct:
            if (k.text == "(") {
                p++;
                ExprP e = expr();
                if (!e || !expect(")")) return nullptr;
                return e;
            }
            break;
        default:
            break;
        }
        fail(k.kind == Tok::End ? "unexpected end of input" : "unexpected '" + k.text + "'");
        return nullptr;
    }

    /* ── 语句 ── */

    bool looks_like_decl()
    {
        size_t q = p;
        /* 限定符 / 精度 */
        while (t[q].kind == Tok::Keyword) {
            const std::string &s = t[q].text;
            if (s == "const" || s == "lowp" || s == "mediump" || s == "highp" || s == "struct" ||
                s == "invariant" || s == "flat" || s == "smooth" || s == "centroid") {
                if (s == "struct") return true;
                q++;
                continue;
            }
            break;
        }
        if (q != p) return true;
        if (!is_type_name(t[q])) return false;
        /* T name / T[N] name；T( 是构造表达式 */
        const Token &n = t[q + 1];
        if (n.kind == Tok::Ident) return true;
        if (n.kind == Tok::Punct && n.text == "[") {
            /* T[...] name 或 T[...](...) */
            size_t r = q + 2;
            int d = 1;
            while (r < t.size() && d > 0) {
                if (t[r].text == "[") d++;
                if (t[r].text == "]") d--;
                if (t[r].kind == Tok::End) return false;
                r++;
            }
            return r < t.size() && t[r].kind == Tok::Ident;
        }
        return false;
    }

    StmtP decl_stmt()
    {
        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Decl;
        s->loc = cur().loc;
        bool any;
        if (!qualifiers(s->qual, any)) return nullptr;
        if (!type_spec(s->type)) return nullptr;
        if (accept(";")) return s; /* 只定义结构体 */
        for (;;) {
            Declarator d;
            d.loc = cur().loc;
            if (cur().kind != Tok::Ident) {
                fail("expected identifier in declaration");
                return nullptr;
            }
            d.name = cur().text;
            p++;
            if (accept("[")) {
                d.is_array = true;
                if (!is("]")) {
                    d.array_size = expr();
                    if (!d.array_size) return nullptr;
                }
                if (!expect("]")) return nullptr;
            }
            if (accept("=")) {
                d.init = assign();
                if (!d.init) return nullptr;
            }
            s->decls.push_back(std::move(d));
            if (accept(",")) continue;
            break;
        }
        if (!expect(";")) return nullptr;
        return s;
    }

    StmtP statement()
    {
        auto s = std::make_unique<Stmt>();
        s->loc = cur().loc;
        if (is("{")) {
            p++;
            s->kind = Stmt::Block;
            while (!is("}")) {
                if (cur().kind == Tok::End) {
                    fail("missing '}'");
                    return nullptr;
                }
                StmtP c = statement();
                if (!c) return nullptr;
                s->body.push_back(std::move(c));
            }
            p++;
            return s;
        }
        if (accept(";")) {
            s->kind = Stmt::Empty;
            return s;
        }
        if (accept("if")) {
            s->kind = Stmt::If;
            if (!expect("(")) return nullptr;
            s->cond = expr();
            if (!s->cond || !expect(")")) return nullptr;
            s->then_s = statement();
            if (!s->then_s) return nullptr;
            if (accept("else")) {
                s->else_s = statement();
                if (!s->else_s) return nullptr;
            }
            return s;
        }
        if (accept("for")) {
            s->kind = Stmt::For;
            if (!expect("(")) return nullptr;
            if (!accept(";")) {
                if (looks_like_decl()) {
                    s->init = decl_stmt();
                    if (!s->init) return nullptr;
                } else {
                    auto es = std::make_unique<Stmt>();
                    es->kind = Stmt::ExprS;
                    es->loc = cur().loc;
                    es->expr = expr();
                    if (!es->expr || !expect(";")) return nullptr;
                    s->init = std::move(es);
                }
            }
            if (!is(";")) {
                s->cond = expr();
                if (!s->cond) return nullptr;
            }
            if (!expect(";")) return nullptr;
            if (!is(")")) {
                s->step = expr();
                if (!s->step) return nullptr;
            }
            if (!expect(")")) return nullptr;
            s->then_s = statement();
            if (!s->then_s) return nullptr;
            return s;
        }
        if (accept("while")) {
            s->kind = Stmt::While;
            if (!expect("(")) return nullptr;
            s->cond = expr();
            if (!s->cond || !expect(")")) return nullptr;
            s->then_s = statement();
            if (!s->then_s) return nullptr;
            return s;
        }
        if (accept("do")) {
            s->kind = Stmt::DoWhile;
            s->then_s = statement();
            if (!s->then_s) return nullptr;
            if (!expect("while") || !expect("(")) return nullptr;
            s->cond = expr();
            if (!s->cond || !expect(")") || !expect(";")) return nullptr;
            return s;
        }
        if (accept("return")) {
            s->kind = Stmt::Return;
            if (!is(";")) {
                s->expr = expr();
                if (!s->expr) return nullptr;
            }
            if (!expect(";")) return nullptr;
            return s;
        }
        if (accept("break")) {
            s->kind = Stmt::Break;
            if (!expect(";")) return nullptr;
            return s;
        }
        if (accept("continue")) {
            s->kind = Stmt::Continue;
            if (!expect(";")) return nullptr;
            return s;
        }
        if (accept("discard")) {
            s->kind = Stmt::Discard;
            if (!expect(";")) return nullptr;
            return s;
        }
        if (is("switch")) {
            fail("switch statements are not supported (use if/else)");
            return nullptr;
        }
        if (looks_like_decl()) {
            return decl_stmt();
        }
        s->kind = Stmt::ExprS;
        s->expr = expr();
        if (!s->expr || !expect(";")) return nullptr;
        return s;
    }

    /* ── 顶层 ── */

    bool param(Param &pm)
    {
        pm.loc = cur().loc;
        for (;;) {
            if (accept("const")) {
                pm.is_const = true;
            } else if (accept("in")) {
                pm.storage = Storage::In;
            } else if (accept("out")) {
                pm.storage = Storage::Out;
            } else if (accept("inout")) {
                pm.storage = Storage::InOut;
            } else if (accept("lowp") || accept("mediump") || accept("highp")) {
            } else {
                break;
            }
        }
        if (!type_spec(pm.type)) return false;
        if (cur().kind == Tok::Ident) {
            pm.name = cur().text;
            p++;
            if (accept("[")) {
                pm.is_array = true;
                pm.array_size = expr();
                if (!pm.array_size || !expect("]")) return false;
            }
        }
        return true;
    }

    bool global()
    {
        if (accept("precision")) {
            skip_precision();
            if (!is_type_name(cur())) return fail("expected type in precision statement");
            p++;
            return expect(";");
        }
        if (accept(";")) return true;

        /* 函数？限定符 + 类型 + 名字 + '(' */
        const size_t save = p;
        Qualifier q;
        bool anyq;
        if (!qualifiers(q, anyq)) return false;
        if (cur().kind == Tok::Ident && !is_type_name(cur()) && is("{")) {
            /* 不会走到 */
        }
        if (cur().kind == Tok::Ident && !is_type_name(cur()) && at(1).text == "{") {
            return fail("uniform/interface blocks are not supported");
        }
        TypeSpec ts;
        if (!type_spec(ts)) return false;
        if (cur().kind == Tok::Ident && at(1).text == "(" && !anyq) {
            auto f = std::make_unique<Function>();
            f->ret = ts;
            f->name = cur().text;
            f->loc = cur().loc;
            p += 2;
            if (!accept(")")) {
                if (cur().kind == Tok::Ident && cur().text == "void" && at(1).text == ")") {
                    p += 2;
                } else {
                    for (;;) {
                        Param pm;
                        if (!param(pm)) return false;
                        f->params.push_back(std::move(pm));
                        if (accept(",")) continue;
                        if (!expect(")")) return false;
                        break;
                    }
                }
            }
            if (accept(";")) {
                Global g;
                g.kind = Global::Func;
                g.func = std::move(f);
                tu.globals.push_back(std::move(g));
                return true;
            }
            if (!is("{")) return fail("expected '{' or ';' after function declaration");
            f->body = statement();
            if (!f->body) return false;
            Global g;
            g.kind = Global::Func;
            g.func = std::move(f);
            tu.globals.push_back(std::move(g));
            return true;
        }
        /* 变量声明：回退重新按声明解析 */
        p = save;
        StmtP d = decl_stmt();
        if (!d) return false;
        Global g;
        g.kind = Global::Var;
        g.decl = std::move(d);
        tu.globals.push_back(std::move(g));
        return true;
    }
};

} // namespace

bool parse(const std::vector<Token> &toks, TranslationUnit &tu, std::string &err)
{
    Parser ps(toks, tu);
    while (ps.cur().kind != Tok::End) {
        if (!ps.global()) {
            err = ps.err;
            return false;
        }
    }
    return true;
}

} // namespace espgpu::glsl
