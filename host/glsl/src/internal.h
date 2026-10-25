/*
 * internal.h — 编译器内部结构：词法单元、AST、DAG
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "espgpu_glsl.h"
#include "espgpu_proto.h"

namespace espgpu::glsl {

struct SrcLoc {
    int line = 0;
    int col = 0;
};

/* ── 词法 ─────────────────────────────────────────────────────────── */

enum class Tok { End, Ident, Int, UInt, Float, Bool, Punct, Keyword };

struct Token {
    Tok kind = Tok::End;
    std::string text;
    double num = 0;
    SrcLoc loc;
};

bool lex(const std::string &src, std::vector<Token> &out, std::string &err);

/* ── AST ──────────────────────────────────────────────────────────── */

struct Expr;
struct Stmt;
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

struct StructDecl;

struct TypeSpec {
    std::string name;        /**< "float" / "vec3" / 结构体名 */
    StructDecl *inline_struct = nullptr;
    bool is_array = false;
    Expr *array_size = nullptr; /**< 归属 owner；nullptr + is_array = 未定长 */
    SrcLoc loc;
};

struct Expr {
    enum Kind {
        Ident,
        IntLit,
        UIntLit,
        FloatLit,
        BoolLit,
        Binary,
        Unary,
        PreInc,
        PostInc,
        Assign,
        Ternary,
        Call,
        Index,
        Field,
        Comma,
    } kind = Ident;
    std::string op;   /**< 运算符 / 赋值运算符 */
    std::string name; /**< 标识符 / 函数名 / 字段名 */
    double num = 0;
    std::vector<ExprP> args;
    TypeSpec ctor;    /**< Call 且是数组构造 float[3](...) 时 */
    bool ctor_array = false;
    SrcLoc loc;
};

enum class Storage { None, Const, In, Out, InOut, Uniform };

struct Qualifier {
    Storage storage = Storage::None;
    int location = -1;
    bool invariant = false;
    bool varying = false;   /**< ES 1.00 varying */
    bool attribute = false; /**< ES 1.00 attribute */
};

struct Declarator {
    std::string name;
    bool is_array = false;
    ExprP array_size;
    ExprP init;
    SrcLoc loc;
};

struct StructMember {
    TypeSpec type;
    std::string name;
    bool is_array = false;
    ExprP array_size;
    SrcLoc loc;
};

struct StructDecl {
    std::string name;
    std::vector<StructMember> members;
    SrcLoc loc;
};

struct Stmt {
    enum Kind {
        Block,
        Decl,
        ExprS,
        If,
        For,
        While,
        DoWhile,
        Return,
        Break,
        Continue,
        Discard,
        Empty,
    } kind = Empty;
    std::vector<StmtP> body;
    ExprP cond;
    ExprP expr;
    ExprP step;
    StmtP init;
    StmtP then_s;
    StmtP else_s;
    /* Decl */
    Qualifier qual;
    TypeSpec type;
    std::vector<Declarator> decls;
    bool new_scope = true;
    SrcLoc loc;
};

struct Param {
    Storage storage = Storage::In; /**< In / Out / InOut */
    bool is_const = false;
    TypeSpec type;
    std::string name;
    bool is_array = false;
    ExprP array_size;
    SrcLoc loc;
};

struct Function {
    TypeSpec ret;
    std::string name;
    std::vector<Param> params;
    StmtP body; /**< nullptr = 只是原型 */
    SrcLoc loc;
};

struct Global {
    enum Kind { Var, Func, Struct, Precision } kind = Var;
    std::unique_ptr<Stmt> decl;       /* Var：Stmt::Decl */
    std::unique_ptr<Function> func;
    std::unique_ptr<StructDecl> st;
};

struct TranslationUnit {
    std::vector<Global> globals;
    std::vector<std::unique_ptr<StructDecl>> structs; /**< 所有结构体（含内联定义） */
    std::vector<ExprP> owned_exprs;                   /**< TypeSpec::array_size 的所有权 */
};

bool parse(const std::vector<Token> &toks, TranslationUnit &tu, std::string &err);

/* ── DAG ──────────────────────────────────────────────────────────── */

enum class NK : uint8_t {
    Const,
    Varying,   /**< FS 输入，index = inputs 下标，宽 = 分量数 */
    Uniform,   /**< index = local 槽，宽 4 */
    Attrib,    /**< VS 输入，index = inputs 下标，宽 4 */
    FragCoord, /**< v6 */
    Op,        /**< VM 运算 */
    Tex,       /**< index = 采样器绑定（Dag::samplers 下标） */
    Construct, /**< 由若干标量拼成的向量 */
    Kill,
};

struct Scalar {
    int node = -1;
    uint8_t comp = 0;
    bool operator==(const Scalar &o) const { return node == o.node && comp == o.comp; }
    bool operator<(const Scalar &o) const
    {
        return node != o.node ? node < o.node : comp < o.comp;
    }
};

struct Operand {
    uint8_t n = 0;
    Scalar s[4];
    bool operator<(const Operand &o) const
    {
        if (n != o.n) return n < o.n;
        for (int i = 0; i < n; i++) {
            if (!(s[i] == o.s[i])) return s[i] < o.s[i];
        }
        return false;
    }
};

struct Node {
    NK kind = NK::Const;
    uint8_t op = 0;    /**< ESPGPU_VM_* */
    uint8_t width = 1; /**< 结果分量数 */
    uint8_t nargs = 0;
    Operand a[3];
    float c = 0;       /**< Const 的值 */
    int index = 0;
};

struct Dag {
    std::vector<Node> nodes;
    std::vector<int> kills;
    std::vector<std::pair<int, int>> samplers; /**< (叶子, 元素) */

    bool has_color = false;
    Operand color;

    Operand position;
    std::vector<Operand> var_out;

    /** 顶点着色器：从输出可达的节点，按拓扑序（主机解释执行用） */
    std::vector<int> live_order;
};

/** 单条 VM 运算的数值语义（折叠、DAG 解释、参考解释器共用） */
void eval_op(uint8_t op, const float a[4], const float b[4], const float c[4], float d[4]);

/** 第三阶段：符号执行 AST，产出 Shader 的接口信息与 DAG */
bool lower(const TranslationUnit &tu, Stage stage, int version, Shader &sh);

/** 第四阶段：片元着色器的模式识别 + VM 代码生成 */
bool codegen_fragment(Shader &sh);

/** 顶点着色器 T&L 识别（在 AST 上做） */
void recognize_vertex(const TranslationUnit &tu, Shader &sh);

} // namespace espgpu::glsl
