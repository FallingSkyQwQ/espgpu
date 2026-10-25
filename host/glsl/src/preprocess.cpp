/*
 * preprocess.cpp — GLSL 预处理器
 *
 * 逐行处理，输出与输入行数完全相同（指令行和非活动行输出空行），
 * 这样词法/语法阶段报的行号就是源文件的行号。
 *
 * 支持：#version #define（对象式 / 函数式，含 ##）#undef #if #ifdef #ifndef
 * #elif #else #endif #error #pragma #extension #line，预定义 GL_ES / __VERSION__ /
 * __LINE__ / __FILE__ / GL_FRAGMENT_PRECISION_HIGH。
 */

#include "internal.h"

#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

namespace espgpu::glsl {

namespace {

struct PPTok {
    enum Kind { Ident, Number, Punct, Space } kind;
    std::string text;
};

bool is_ident_start(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_ident_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::vector<PPTok> pp_tokenize(const std::string &s)
{
    std::vector<PPTok> out;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            size_t j = i;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\f' ||
                                    s[j] == '\v')) {
                j++;
            }
            out.push_back({PPTok::Space, " "});
            i = j;
        } else if (is_ident_start(c)) {
            size_t j = i;
            while (j < s.size() && is_ident_char(s[j])) {
                j++;
            }
            out.push_back({PPTok::Ident, s.substr(i, j - i)});
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                   (c == '.' && i + 1 < s.size() &&
                    std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            size_t j = i;
            while (j < s.size()) {
                const char d = s[j];
                if (is_ident_char(d) || d == '.') {
                    j++;
                } else if ((d == '+' || d == '-') && j > i &&
                           (s[j - 1] == 'e' || s[j - 1] == 'E') &&
                           !(s[i] == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X'))) {
                    j++;
                } else {
                    break;
                }
            }
            out.push_back({PPTok::Number, s.substr(i, j - i)});
            i = j;
        } else {
            static const char *two[] = {"##", "&&", "||", "==", "!=", "<=", ">=", "<<", ">>",
                                        "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
                                        "^=", "^^"};
            bool matched = false;
            if (i + 1 < s.size()) {
                for (const char *t : two) {
                    if (s[i] == t[0] && s[i + 1] == t[1]) {
                        // <<= >>=
                        if ((t[0] == '<' || t[0] == '>') && t[1] == t[0] && i + 2 < s.size() &&
                            s[i + 2] == '=') {
                            out.push_back({PPTok::Punct, s.substr(i, 3)});
                            i += 3;
                        } else {
                            out.push_back({PPTok::Punct, std::string(t, 2)});
                            i += 2;
                        }
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                out.push_back({PPTok::Punct, std::string(1, c)});
                i++;
            }
        }
    }
    return out;
}

struct Macro {
    bool function = false;
    std::vector<std::string> params;
    std::vector<PPTok> body;
};

struct PP {
    std::map<std::string, Macro> macros;
    std::string err;
    int line = 0;
    int version = 100;

    bool fail(const std::string &m)
    {
        if (err.empty()) {
            err = std::to_string(line) + ":1: error: " + m;
        }
        return false;
    }

    static std::string join(const std::vector<PPTok> &t)
    {
        std::string s;
        for (const auto &x : t) {
            s += x.text;
        }
        return s;
    }

    std::vector<PPTok> expand(const std::vector<PPTok> &in, const std::set<std::string> &off,
                              int depth)
    {
        if (depth > 64) {
            fail("macro expansion too deep");
            return in;
        }
        std::vector<PPTok> out;
        for (size_t i = 0; i < in.size(); i++) {
            const PPTok &t = in[i];
            if (t.kind != PPTok::Ident || off.count(t.text)) {
                out.push_back(t);
                continue;
            }
            if (t.text == "__LINE__") {
                out.push_back({PPTok::Number, std::to_string(line)});
                continue;
            }
            if (t.text == "__FILE__") {
                out.push_back({PPTok::Number, "0"});
                continue;
            }
            if (t.text == "__VERSION__") {
                out.push_back({PPTok::Number, std::to_string(version)});
                continue;
            }
            auto it = macros.find(t.text);
            if (it == macros.end()) {
                out.push_back(t);
                continue;
            }
            const Macro &m = it->second;
            std::set<std::string> off2 = off;
            off2.insert(t.text);
            if (!m.function) {
                auto r = expand(m.body, off2, depth + 1);
                out.insert(out.end(), r.begin(), r.end());
                continue;
            }
            /* 函数式：找 '(' */
            size_t j = i + 1;
            while (j < in.size() && in[j].kind == PPTok::Space) {
                j++;
            }
            if (j >= in.size() || in[j].text != "(") {
                out.push_back(t); /* 不是调用，原样输出 */
                continue;
            }
            std::vector<std::vector<PPTok>> args(1);
            int pd = 0;
            size_t k = j + 1;
            bool closed = false;
            for (; k < in.size(); k++) {
                const std::string &x = in[k].text;
                if (in[k].kind == PPTok::Punct) {
                    if (x == "(") {
                        pd++;
                    } else if (x == ")") {
                        if (pd == 0) {
                            closed = true;
                            break;
                        }
                        pd--;
                    } else if (x == "," && pd == 0) {
                        args.emplace_back();
                        continue;
                    }
                }
                args.back().push_back(in[k]);
            }
            if (!closed) {
                fail("unterminated macro invocation '" + t.text + "'");
                return out;
            }
            if (m.params.empty() && args.size() == 1) {
                bool blank = true;
                for (const auto &x : args[0]) {
                    if (x.kind != PPTok::Space) {
                        blank = false;
                    }
                }
                if (blank) {
                    args.clear();
                }
            }
            if (args.size() != m.params.size()) {
                fail("macro '" + t.text + "' expects " + std::to_string(m.params.size()) +
                     " arguments, got " + std::to_string(args.size()));
                return out;
            }
            /* 替换：## 两侧用未展开的实参，其它位置用展开后的实参 */
            std::vector<PPTok> body;
            for (size_t b = 0; b < m.body.size(); b++) {
                const PPTok &bt = m.body[b];
                int pidx = -1;
                if (bt.kind == PPTok::Ident) {
                    for (size_t p = 0; p < m.params.size(); p++) {
                        if (m.params[p] == bt.text) {
                            pidx = static_cast<int>(p);
                        }
                    }
                }
                if (pidx < 0) {
                    body.push_back(bt);
                    continue;
                }
                auto near_paste = [&](int dir) {
                    long q = static_cast<long>(b) + dir;
                    while (q >= 0 && q < static_cast<long>(m.body.size()) &&
                           m.body[static_cast<size_t>(q)].kind == PPTok::Space) {
                        q += dir;
                    }
                    return q >= 0 && q < static_cast<long>(m.body.size()) &&
                           m.body[static_cast<size_t>(q)].text == "##";
                };
                if (near_paste(-1) || near_paste(1)) {
                    body.insert(body.end(), args[static_cast<size_t>(pidx)].begin(),
                                args[static_cast<size_t>(pidx)].end());
                } else {
                    auto e = expand(args[static_cast<size_t>(pidx)], off, depth + 1);
                    body.insert(body.end(), e.begin(), e.end());
                }
            }
            /* 处理 ## */
            std::vector<PPTok> pasted;
            for (size_t b = 0; b < body.size(); b++) {
                if (body[b].text == "##" && body[b].kind == PPTok::Punct) {
                    while (!pasted.empty() && pasted.back().kind == PPTok::Space) {
                        pasted.pop_back();
                    }
                    size_t n = b + 1;
                    while (n < body.size() && body[n].kind == PPTok::Space) {
                        n++;
                    }
                    if (!pasted.empty() && n < body.size()) {
                        std::string s = pasted.back().text + body[n].text;
                        auto re = pp_tokenize(s);
                        pasted.pop_back();
                        pasted.insert(pasted.end(), re.begin(), re.end());
                        b = n;
                    }
                    continue;
                }
                pasted.push_back(body[b]);
            }
            auto r = expand(pasted, off2, depth + 1);
            out.insert(out.end(), r.begin(), r.end());
            i = k;
        }
        return out;
    }

    /* ── #if 表达式 ── */
    std::vector<PPTok> et;
    size_t ep = 0;

    const PPTok *peek()
    {
        while (ep < et.size() && et[ep].kind == PPTok::Space) {
            ep++;
        }
        return ep < et.size() ? &et[ep] : nullptr;
    }

    bool accept(const char *s)
    {
        const PPTok *t = peek();
        if (t && t->text == s) {
            ep++;
            return true;
        }
        return false;
    }

    long long primary()
    {
        const PPTok *t = peek();
        if (!t) {
            fail("unexpected end of #if expression");
            return 0;
        }
        if (accept("(")) {
            long long v = expr(0);
            if (!accept(")")) {
                fail("missing ')' in #if expression");
            }
            return v;
        }
        if (accept("!")) {
            return !primary();
        }
        if (accept("~")) {
            return ~primary();
        }
        if (accept("-")) {
            return -primary();
        }
        if (accept("+")) {
            return primary();
        }
        ep++;
        if (t->kind == PPTok::Number) {
            std::string s = t->text;
            while (!s.empty() && (s.back() == 'u' || s.back() == 'U')) {
                s.pop_back();
            }
            return std::strtoll(s.c_str(), nullptr, 0);
        }
        if (t->kind == PPTok::Ident) {
            if (t->text == "true") {
                return 1;
            }
            return 0; /* 未定义的标识符按 0 处理（宽松） */
        }
        fail("bad token '" + t->text + "' in #if expression");
        return 0;
    }

    static int prec(const std::string &op)
    {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=") return 6;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 7;
        if (op == "<<" || op == ">>") return 8;
        if (op == "+" || op == "-") return 9;
        if (op == "*" || op == "/" || op == "%") return 10;
        return -1;
    }

    long long expr(int min_prec)
    {
        long long lhs = primary();
        for (;;) {
            const PPTok *t = peek();
            if (!t || t->kind != PPTok::Punct) {
                break;
            }
            const std::string op = t->text;
            const int p = prec(op);
            if (p < 0 || p < min_prec) {
                break;
            }
            ep++;
            long long rhs = expr(p + 1);
            if (op == "||") lhs = lhs || rhs;
            else if (op == "&&") lhs = lhs && rhs;
            else if (op == "|") lhs |= rhs;
            else if (op == "^") lhs ^= rhs;
            else if (op == "&") lhs &= rhs;
            else if (op == "==") lhs = lhs == rhs;
            else if (op == "!=") lhs = lhs != rhs;
            else if (op == "<") lhs = lhs < rhs;
            else if (op == ">") lhs = lhs > rhs;
            else if (op == "<=") lhs = lhs <= rhs;
            else if (op == ">=") lhs = lhs >= rhs;
            else if (op == "<<") lhs <<= rhs;
            else if (op == ">>") lhs >>= rhs;
            else if (op == "+") lhs += rhs;
            else if (op == "-") lhs -= rhs;
            else if (op == "*") lhs *= rhs;
            else if (op == "/" || op == "%") {
                if (rhs == 0) {
                    fail("division by zero in #if");
                    return 0;
                }
                lhs = (op == "/") ? lhs / rhs : lhs % rhs;
            }
        }
        return lhs;
    }

    bool eval_if(const std::vector<PPTok> &toks, bool &out)
    {
        /* 先处理 defined，再宏展开 */
        std::vector<PPTok> pre;
        for (size_t i = 0; i < toks.size(); i++) {
            if (toks[i].kind == PPTok::Ident && toks[i].text == "defined") {
                size_t j = i + 1;
                while (j < toks.size() && toks[j].kind == PPTok::Space) j++;
                bool paren = false;
                if (j < toks.size() && toks[j].text == "(") {
                    paren = true;
                    j++;
                    while (j < toks.size() && toks[j].kind == PPTok::Space) j++;
                }
                if (j >= toks.size() || toks[j].kind != PPTok::Ident) {
                    return fail("'defined' without identifier");
                }
                const std::string &name = toks[j].text;
                const bool d = macros.count(name) || name == "GL_ES" || name == "__VERSION__" ||
                               name == "__LINE__" || name == "__FILE__";
                j++;
                if (paren) {
                    while (j < toks.size() && toks[j].kind == PPTok::Space) j++;
                    if (j >= toks.size() || toks[j].text != ")") {
                        return fail("missing ')' after defined");
                    }
                    j++;
                }
                pre.push_back({PPTok::Number, d ? "1" : "0"});
                i = j - 1;
                continue;
            }
            pre.push_back(toks[i]);
        }
        et = expand(pre, {}, 0);
        ep = 0;
        long long v = expr(0);
        if (peek() != nullptr) {
            return fail("trailing tokens in #if expression");
        }
        out = v != 0;
        return err.empty();
    }
};

/* 拼接续行、去注释，保留换行 */
std::string strip_comments(const std::string &src)
{
    std::string s;
    s.reserve(src.size());
    int pending = 0; /* 续行吞掉的换行，在逻辑行结束后补回，保持行号 */
    for (size_t i = 0; i < src.size(); i++) {
        if (src[i] == '\\' && i + 1 < src.size() && src[i + 1] == '\n') {
            i++;
            s += ' ';
            pending++;
            continue;
        }
        s += src[i];
        if (src[i] == '\n') {
            s.append(static_cast<size_t>(pending), '\n');
            pending = 0;
        }
    }
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') i++;
        } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            o += ' ';
            while (i < s.size() && !(s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/')) {
                if (s[i] == '\n') o += '\n';
                i++;
            }
            i += 2;
        } else {
            o += s[i++];
        }
    }
    return o;
}

} // namespace

bool preprocess(const std::string &src, std::string &out, std::string &err, int *version_out)
{
    PP pp;
    const std::string text = strip_comments(src);

    std::vector<std::string> lines;
    {
        size_t a = 0;
        for (size_t i = 0; i <= text.size(); i++) {
            if (i == text.size() || text[i] == '\n') {
                lines.push_back(text.substr(a, i - a));
                a = i + 1;
            }
        }
    }

    Macro gles;
    gles.body = {{PPTok::Number, "1"}};
    pp.macros["GL_ES"] = gles;
    pp.macros["GL_FRAGMENT_PRECISION_HIGH"] = gles;

    struct Cond {
        bool parent_active;
        bool taken;  /* 某个分支已经取过 */
        bool active;
        bool seen_else;
    };
    std::vector<Cond> stack;
    bool seen_code = false;

    out.clear();
    for (size_t ln = 0; ln < lines.size(); ln++) {
        pp.line = static_cast<int>(ln) + 1;
        const std::string &L = lines[ln];
        if (ln + 1 == lines.size() && L.empty()) break; /* 文件末尾的换行不再多出一行 */
        size_t p = 0;
        while (p < L.size() && (L[p] == ' ' || L[p] == '\t' || L[p] == '\r')) p++;
        const bool active = stack.empty() || stack.back().active;

        if (p < L.size() && L[p] == '#') {
            auto toks = pp_tokenize(L.substr(p + 1));
            size_t ti = 0;
            while (ti < toks.size() && toks[ti].kind == PPTok::Space) ti++;
            const std::string dir = ti < toks.size() ? toks[ti].text : "";
            ti++;
            std::vector<PPTok> rest(toks.begin() + static_cast<long>(std::min(ti, toks.size())),
                                    toks.end());
            auto first_ident = [&]() -> std::string {
                for (const auto &t : rest) {
                    if (t.kind != PPTok::Space) return t.kind == PPTok::Ident ? t.text : "";
                }
                return "";
            };

            if (dir == "ifdef" || dir == "ifndef") {
                const std::string n = first_ident();
                if (n.empty() && active) {
                    pp.fail("#" + dir + " needs an identifier");
                }
                bool d = pp.macros.count(n) > 0;
                if (dir == "ifndef") d = !d;
                stack.push_back({active, d, active && d, false});
            } else if (dir == "if") {
                bool v = false;
                if (active && !pp.eval_if(rest, v)) {
                    break;
                }
                stack.push_back({active, v, active && v, false});
            } else if (dir == "elif") {
                if (stack.empty() || stack.back().seen_else) {
                    pp.fail("#elif without #if");
                    break;
                }
                Cond &c = stack.back();
                bool v = false;
                if (c.parent_active && !c.taken) {
                    if (!pp.eval_if(rest, v)) break;
                }
                c.active = c.parent_active && !c.taken && v;
                c.taken = c.taken || c.active;
            } else if (dir == "else") {
                if (stack.empty() || stack.back().seen_else) {
                    pp.fail("#else without #if");
                    break;
                }
                Cond &c = stack.back();
                c.active = c.parent_active && !c.taken;
                c.taken = true;
                c.seen_else = true;
            } else if (dir == "endif") {
                if (stack.empty()) {
                    pp.fail("#endif without #if");
                    break;
                }
                stack.pop_back();
            } else if (!active) {
                /* 非活动区里的其它指令忽略 */
            } else if (dir == "version") {
                if (seen_code) {
                    pp.fail("#version must be the first directive");
                    break;
                }
                std::string num;
                std::string prof;
                for (const auto &t : rest) {
                    if (t.kind == PPTok::Number && num.empty()) num = t.text;
                    else if (t.kind == PPTok::Ident) prof = t.text;
                }
                pp.version = std::atoi(num.c_str());
                if (pp.version != 100 && pp.version != 300 && pp.version != 310 &&
                    pp.version != 320) {
                    pp.fail("unsupported #version " + num + " (expected 100 or 300 es)");
                    break;
                }
                if (pp.version >= 300 && prof != "es") {
                    pp.fail("#version " + num + " requires the 'es' profile");
                    break;
                }
            } else if (dir == "define") {
                size_t i = 0;
                while (i < rest.size() && rest[i].kind == PPTok::Space) i++;
                if (i >= rest.size() || rest[i].kind != PPTok::Ident) {
                    pp.fail("#define needs a name");
                    break;
                }
                const std::string name = rest[i].text;
                if (name.rfind("GL_", 0) == 0 || name.find("__") != std::string::npos) {
                    if (name != "GL_ES") {
                        /* 规范保留，但很多着色器会这么写；只警告式地接受 */
                    }
                }
                i++;
                Macro m;
                if (i < rest.size() && rest[i].text == "(") { /* 紧贴名字的 ( 才是函数式 */
                    m.function = true;
                    i++;
                    for (;;) {
                        while (i < rest.size() && rest[i].kind == PPTok::Space) i++;
                        if (i < rest.size() && rest[i].text == ")") {
                            i++;
                            break;
                        }
                        if (i >= rest.size() || rest[i].kind != PPTok::Ident) {
                            pp.fail("bad parameter list in #define " + name);
                            break;
                        }
                        m.params.push_back(rest[i].text);
                        i++;
                        while (i < rest.size() && rest[i].kind == PPTok::Space) i++;
                        if (i < rest.size() && rest[i].text == ",") {
                            i++;
                            continue;
                        }
                        if (i < rest.size() && rest[i].text == ")") {
                            i++;
                            break;
                        }
                        pp.fail("bad parameter list in #define " + name);
                        break;
                    }
                    if (!pp.err.empty()) break;
                }
                while (i < rest.size() && rest[i].kind == PPTok::Space) i++;
                m.body.assign(rest.begin() + static_cast<long>(i), rest.end());
                while (!m.body.empty() && m.body.back().kind == PPTok::Space) m.body.pop_back();
                pp.macros[name] = m;
            } else if (dir == "undef") {
                pp.macros.erase(first_ident());
            } else if (dir == "error") {
                pp.fail("#error" + PP::join(rest));
                break;
            } else if (dir == "pragma" || dir == "extension" || dir == "line" || dir.empty()) {
                /* 接受并忽略 */
            } else {
                pp.fail("unknown preprocessor directive #" + dir);
                break;
            }
            seen_code = seen_code || (dir != "version" && !dir.empty() && dir != "extension" &&
                                      dir != "pragma");
            out += '\n';
            continue;
        }

        if (active) {
            bool nonblank = false;
            for (char ch : L) {
                if (!std::isspace(static_cast<unsigned char>(ch))) nonblank = true;
            }
            seen_code = seen_code || nonblank;
            auto e = pp.expand(pp_tokenize(L), {}, 0);
            if (!pp.err.empty()) break;
            out += PP::join(e);
        }
        out += '\n';
    }

    if (pp.err.empty() && !stack.empty()) {
        pp.fail("missing #endif");
    }
    if (version_out) {
        *version_out = pp.version;
    }
    err = pp.err;
    return err.empty();
}

} // namespace espgpu::glsl
