/*
 * espgpu_glsl.h — GLSL ES 3.00 / 1.00 编译器（M6）
 *
 * 流程：预处理 → 词法 → 递归下降语法分析 → 符号执行（类型检查 + 常量折叠 +
 * 函数内联 + 循环展开 + if/else 谓词化）得到一张无分支的 DAG，然后：
 *
 *   片元着色器：先做模式识别，命中设备内置着色模式（ESPGPU_SHADE_*）就直接
 *               折叠成 PIPELINE 状态；否则生成 espgpu_vm.h 的字节码。
 *   顶点着色器：识别 `gl_Position = U0 * U1 * ... * pos` 且 varying 全是属性直通
 *               的写法 → 走设备 T&L；否则在主机上逐顶点解释执行 DAG。
 *
 * 纯 C++20，不依赖 Windows，可以在任何平台上单元测试。
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "espgpu_vm.h"

namespace espgpu::glsl {

enum class Stage { Vertex, Fragment };

/** GL 类型枚举值（与 GLES 头文件的 GL_FLOAT_VEC4 等一致），供 glGetActiveUniform 使用 */
enum : uint32_t {
    GLT_FLOAT = 0x1406,
    GLT_INT = 0x1404,
    GLT_UINT = 0x1405,
    GLT_BOOL = 0x8B56,
    GLT_FLOAT_VEC2 = 0x8B50,
    GLT_FLOAT_VEC3 = 0x8B51,
    GLT_FLOAT_VEC4 = 0x8B52,
    GLT_INT_VEC2 = 0x8B53,
    GLT_INT_VEC3 = 0x8B54,
    GLT_INT_VEC4 = 0x8B55,
    GLT_BOOL_VEC2 = 0x8B57,
    GLT_BOOL_VEC3 = 0x8B58,
    GLT_BOOL_VEC4 = 0x8B59,
    GLT_UINT_VEC2 = 0x8DC6,
    GLT_UINT_VEC3 = 0x8DC7,
    GLT_UINT_VEC4 = 0x8DC8,
    GLT_FLOAT_MAT2 = 0x8B5A,
    GLT_FLOAT_MAT3 = 0x8B5B,
    GLT_FLOAT_MAT4 = 0x8B5C,
    GLT_SAMPLER_2D = 0x8B5E,
};

/** 一个"叶子" uniform（结构体已展平成 s.a / s[1].b 这样的名字） */
struct UniformLeaf {
    std::string name;     /**< 不含 [0] 的基名 */
    uint32_t gl_type = 0; /**< GLT_* */
    int array_size = 1;   /**< 非数组为 1 */
    int columns = 1;      /**< 每个元素占几个 vec4 槽（matN 为 N） */
    int components = 1;   /**< 每个槽有效分量数 */
    bool is_sampler = false;
    bool is_int = false;  /**< int / uint / bool 族：glUniform*i 写入 */
    int local_slot = -1;  /**< 本着色器内第一个 vec4 槽（采样器为 -1） */
    bool used = false;
};

/** 顶点输入 / 片元输入 / 输出 */
struct InOut {
    std::string name;
    int components = 4; /**< 1..4 */
    int location = -1;  /**< layout(location) 或 -1 */
    bool is_int = false;
    bool used = false;
};

/** 片元阶段的设备映射 */
struct FragmentPlan {
    bool program = false; /**< true = VM 字节码，false = 内置着色模式 */

    /* 内置模式 */
    uint8_t shade_mode = 0;
    bool alpha_test = false;
    uint8_t alpha_func = 7; /**< ESPGPU_CMP_* */
    uint8_t alpha_ref = 0;
    int color_input = -1;        /**< 作为顶点色的 varying（inputs 下标）；分量数 3 时 alpha 恒为 1 */
    /**
     * 颜色里与片元无关的因子：c 槽（fragment_consts 填好的常量表下标）。逐分量连乘得 K。
     * 有 color_input 时顶点色 = varying × K（运行时逐顶点乘，插值是线性的所以等价）；
     * 否则 K 就是 COLOR 命令下发的常量色。空 = (1,1,1,1)。
     */
    std::vector<int> color_slots;
    bool color_white = false;    /**< 模式需要颜色但着色器里既没有顶点色也没有常量因子：常量白 */

    /** varying → 设备属性槽（ESPGPU_ATTR_*）。同一 varying 可以出现多次 */
    std::vector<std::pair<int, int>> varying_slots;

    /** 设备纹理单元 → (采样器 uniform 叶子下标, 数组元素) */
    std::vector<std::pair<int, int>> unit_sampler;

    /* VM */
    std::vector<espgpu_vm_insn_t> code;
    uint8_t varying_count = 0;
    int uniform_slots = 0; /**< c0..c(uniform_slots-1) 是 uniform，之后是常量与主机预计算值 */
    int const_slots = 0;   /**< 常量表总槽数（含 uniform）；用 fragment_consts() 生成内容 */
    int preshader_nodes = 0; /**< 每次绘制在主机上预计算的运算数（只依赖 uniform 的子表达式） */
};

/** 顶点阶段的设备映射 */
struct VertexPlan {
    bool tnl = false;
    std::vector<int> mvp_uniforms; /**< 左乘顺序：M = U[0] * U[1] * ...（叶子下标，mat4） */
    int pos_input = -1;
    int pos_components = 4;
    /** 每个输出 varying：直通的输入属性下标与分量数（-1 = 非直通） */
    std::vector<int> varying_input;
};

struct Dag; /* 内部 */

struct Shader {
    bool ok = false;
    std::string log; /**< "line:col: error: ..." 每行一条 */
    Stage stage = Stage::Vertex;
    int version = 100;

    std::vector<UniformLeaf> uniforms;
    std::vector<InOut> inputs;  /**< VS：属性；FS：varying */
    std::vector<InOut> outputs; /**< VS：varying；FS：颜色输出 */
    int uniform_slot_count = 0; /**< local_slot 总数 */

    FragmentPlan frag;
    VertexPlan vert;

    std::shared_ptr<Dag> dag;
};

Shader compile(Stage stage, const std::string &source);

/** 仅预处理（测试用）。出错返回 false，err 写错误信息。 */
bool preprocess(const std::string &src, std::string &out, std::string &err, int *version);

/* ── 主机端执行 ───────────────────────────────────────────────────── */

/**
 * 顶点着色器解释执行。
 *   inputs[i]   = 第 i 个输入属性的值（缺省分量调用方补 0,0,0,1）
 *   uniforms[k] = 第 k 个 local 槽
 *   out_pos     = gl_Position
 *   out_var[j]  = 第 j 个输出 varying
 */
void run_vertex(const Shader &vs, const float (*inputs)[4], const float (*uniforms)[4],
                float out_pos[4], float (*out_var)[4]);

/**
 * 批量执行 n 个顶点：只依赖 uniform 的子表达式整批只算一次。
 *   inputs = n × vs.inputs.size() 个 vec4（顶点主序）
 *   out    = n × (1 + vs.outputs.size()) 个 vec4：每个顶点先 gl_Position，再各输出 varying
 */
void run_vertex_batch(const Shader &vs, const float (*uniforms)[4], size_t n, const float (*inputs)[4],
                      float (*out)[4]);

/**
 * 片元着色器的常量表 c[0 .. frag.const_slots)：uniform 槽原样拷贝，其后是编译期常量
 * 与主机预计算（preshader）的值。uniforms[k] = 第 k 个 local 槽。每次绘制前调用一次；
 * VM 模式整表下发 SET_UNIFORMS，内置模式用它取 color_slots。
 */
void fragment_consts(const Shader &fs, const float (*uniforms)[4], float (*c)[4]);

/* ── VM 工具 ───────────────────────────────────────────────────────── */

using TexFn = void (*)(int unit, float u, float v, float out[4], void *user);

/**
 * 参考解释器，语义与 shared/espgpu_vm.h 完全一致（测试 / 对拍设备实现用）。
 * 返回 false 表示片元被 KIL 丢弃。
 */
bool vm_run(const espgpu_vm_insn_t *code, size_t n, const float v[ESPGPU_VM_VARYINGS][4],
            const float c[ESPGPU_VM_CONSTS][4], TexFn tex, void *user, float out[4]);

std::string vm_disassemble(const espgpu_vm_insn_t *code, size_t n);

/* ── 链接 ─────────────────────────────────────────────────────────── */

struct ProgramUniform {
    std::string name;
    uint32_t gl_type = 0;
    int array_size = 1;
    int columns = 1;
    int components = 1;
    bool is_sampler = false;
    bool is_int = false;
    int storage = 0;  /**< 程序 uniform 存储里的第一个 vec4 槽 */
    int vs_leaf = -1; /**< 在 VS / FS 的叶子下标 */
    int fs_leaf = -1;
};

struct Program {
    bool ok = false;
    std::string log;
    std::vector<ProgramUniform> uniforms; /**< 可见的在前；名字以 '@' 开头的是编译器内部的隐藏 uniform */
    int visible_uniforms = 0;
    int storage_slots = 0;
    /** FS 输入 j 来自 VS 输出 vs_out_for_fs_in[j]（-1 = VS 没写，取 0） */
    std::vector<int> vs_out_for_fs_in;
};

Program link(const Shader &vs, const Shader &fs);

} // namespace espgpu::glsl
