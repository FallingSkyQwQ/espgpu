/*
 * espgpu_proto.h — ESP32-S3-GPU 线协议
 *
 * 纯 C 定义，固件与 Windows 端共用。不依赖除 <stdint.h> 以外的任何东西。
 * 所有多字节字段一律小端。
 *
 * 分层：
 *   1. Chunk 层   ：EP1/EP2 上的最外层分帧，负责重组与错误检测
 *   2. Command 层 ：Chunk 内的 GPU 命令流（仅 H2D）
 *   3. Frame 层   ：Chunk 内的编码像素数据（仅 D2H）
 *   4. Control 层 ：请求/响应（双向，走 CTRL chunk）
 *
 * 设计取向：设备端只负责"通用 GPU"语义（管线状态 + 顶点 + 纹理 + 绘制），
 * 不承载任何 OpenGL 特有的概念（矩阵栈、显示列表、texenv 解析等）；
 * 那些全部由 host 前端翻译成这里的原语。这样 GL 1.2 / GLES 3.0 / 未来的
 * 其它前端可以共用同一套设备实现。
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 线上格式一律紧凑排列，不允许编译器插入对齐填充 */
#pragma pack(push, 1)

/* ════════════════════════════════════════════════════════════════════
 * 版本与常量
 * ════════════════════════════════════════════════════════════════════ */

#define ESPGPU_PROTO_VERSION 1u

/** Chunk 魔数（按小端写入，读作 ASCII 即 "ESPG" / "ESPF"） */
#define ESPGPU_MAGIC_H2D 0x47505345u /* host -> device */
#define ESPGPU_MAGIC_D2H 0x46505345u /* device -> host */

/** 单个 Chunk 载荷上限。帧数据可跨多个 Chunk 分片。 */
#define ESPGPU_CHUNK_MAX_PAYLOAD (32u * 1024u)

/** 每帧最多分片数（320x200 RGB565 裸数据 128KB，32KB/片 → 4 片） */
#define ESPGPU_MAX_FRAGMENTS 16u

#define ESPGPU_MAX_VERTEX_ATTRIBS 8u
#define ESPGPU_MAX_TEXTURE_UNITS 4u
#define ESPGPU_MAX_TEXTURES 4096u

/** USB 标识。固件与主机端共用，避免两边写死不一致。 */
#define ESPGPU_USB_VID 0x303Au
#define ESPGPU_USB_PID 0x8201u

/* ════════════════════════════════════════════════════════════════════
 * 1. Chunk 层
 * ════════════════════════════════════════════════════════════════════ */

/** Chunk 类型 —— H2D */
enum {
    ESPGPU_CHUNK_CMD = 0x01,  /**< GPU 命令流，载荷是一串 espgpu_cmd_t */
    ESPGPU_CHUNK_DATA = 0x02, /**< 二进制 blob（纹理 / 顶点缓冲 / 固件），aux=流 ID */
    ESPGPU_CHUNK_CTRL = 0x03, /**< 控制请求，载荷是 espgpu_ctrl_req_t + 参数 */
};

/** Chunk 类型 —— D2H */
enum {
    ESPGPU_CHUNK_FRAME = 0x81,    /**< 编码像素，载荷是 espgpu_frame_hdr_t + 数据 */
    ESPGPU_CHUNK_EVENT = 0x82,    /**< 事件（vsync / 错误 / 状态） */
    ESPGPU_CHUNK_CTRL_RESP = 0x83,/**< 控制响应 */
    ESPGPU_CHUNK_LOG = 0x84,      /**< 文本日志 */
    ESPGPU_CHUNK_CLUT = 0x85,     /**< 256 × RGB565 调色板（512 字节），变化时主动推送 */
};

/** header.flags 位定义 */
enum {
    ESPGPU_F_MORE = 1u << 0,    /**< 后续还有同类型的 Chunk（分片） */
    ESPGPU_F_CRC = 1u << 1,     /**< 末尾附 2 字节 CRC16-CCITT */
    ESPGPU_F_ACK = 1u << 2,     /**< 请求对端回 ACK */
    ESPGPU_F_URGENT = 1u << 3,  /**< 优先发送 */
    ESPGPU_F_LZ = 1u << 4,      /**< 载荷经 espgpu_lz 压缩，前 4 字节为解压后长度（仅 CMD / DATA） */
};

/** 最外层分帧头，固定 12 字节。 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t seq;   /**< 单调递增，用于检测丢片与重排 */
    uint16_t length;/**< 后续载荷字节数（不含 CRC） */
    uint8_t type;   /**< ESPGPU_CHUNK_* */
    uint8_t flags;  /**< ESPGPU_F_* */
} espgpu_chunk_hdr_t;

#define ESPGPU_CHUNK_HDR_SIZE 12u
#define ESPGPU_CRC_SIZE 2u

/* ════════════════════════════════════════════════════════════════════
 * 2. Command 层（H2D）
 * ════════════════════════════════════════════════════════════════════ */

/** 命令头，固定 4 字节。命令体紧随其后，长度 = size。 */
typedef struct {
    uint8_t op;    /**< ESPGPU_OP_* */
    uint8_t flags; /**< 操作数标志，见各命令 */
    uint16_t size; /**< 命令体字节数 */
} espgpu_cmd_t;

enum {
    ESPGPU_OP_NOP = 0x00,
    ESPGPU_OP_PIPELINE = 0x01,      /**< 设置管线状态 */
    ESPGPU_OP_SET_MATRIX = 0x02,    /**< 设置 4x4 矩阵 */
    ESPGPU_OP_VERTEX_LAYOUT = 0x03, /**< 定义顶点属性布局 */
    ESPGPU_OP_BIND_TEXTURE = 0x04,  /**< 绑定纹理到纹理单元 */
    ESPGPU_OP_UPLOAD_TEXTURE = 0x05,/**< 上传纹理（可走 DATA chunk） */
    ESPGPU_OP_DRAW = 0x06,          /**< 绘制 */
    ESPGPU_OP_CLEAR = 0x07,         /**< 清除颜色/深度 */
    ESPGPU_OP_VIEWPORT = 0x08,      /**< 视口 */
    ESPGPU_OP_SCISSOR = 0x09,       /**< 裁剪矩形 */
    ESPGPU_OP_SET_CLUT = 0x0A,      /**< 设置调色板 */
    ESPGPU_OP_FBO = 0x0B,           /**< 离屏目标绑定/解绑/拷贝 */
    ESPGPU_OP_PRESENT = 0x0C,       /**< 呈现当前帧 */
    ESPGPU_OP_QUERY = 0x0D,         /**< 查询统计 */
    ESPGPU_OP_FENCE = 0x0E,         /**< 同步点，设备回 FLUSH 事件 */
    ESPGPU_OP_PIPELINE2D = 0x10,    /**< M2：2D 管线子集 */
    /* ── 以下为 M2-M6 追加 ── */
    ESPGPU_OP_TEX_PALETTE = 0x11,   /**< 纹理调色板（GL_EXT_paletted_texture） */
    ESPGPU_OP_DELETE_TEXTURE = 0x12,/**< 释放纹理（主机侧 LRU 镜像主动换出） */
    ESPGPU_OP_COLOR = 0x13,         /**< 常量顶点色（布局里没有颜色属性时使用） */
    ESPGPU_OP_LIGHTING = 0x14,      /**< 顶点光照参数（M5） */
    ESPGPU_OP_LOAD_PROGRAM = 0x15,  /**< 上传片元 VM 字节码（M6） */
    ESPGPU_OP_SET_UNIFORMS = 0x16,  /**< 片元 VM 常量寄存器（M6） */
    ESPGPU_OP_DEPTH_RANGE = 0x17,   /**< glDepthRange */
};

/** 命令头 flags —— DRAW */
enum {
    ESPGPU_DRAWF_3D = 1u << 0, /**< 使用 VERTEX_LAYOUT 定义的顶点格式并走 T&L；未置位 = M2 的 2D 顶点 */
};

/** 命令头 flags —— BIND_TEXTURE */
enum {
    ESPGPU_BINDF_WRAP_V = 1u << 0, /**< espgpu_bind_texture_t.pad 的低字节是 t 方向 wrap（否则 u/v 都用 wrap） */
};

/** 命令头 flags —— UPLOAD_TEXTURE */
enum {
    ESPGPU_TEXF_PRIORITY = 1u << 0, /**< 预加载：LRU 换出时最后考虑 */
};

/* ── 像素格式 ─────────────────────────────────────────────────────── */

/**
 * 设备侧统一把这些格式统一展开成 RGB565 或索引色参与光栅化。
 * 纹理可以额外保留 8 位索引形态以节省 PSRAM 与上传带宽。
 */
enum {
    ESPGPU_FMT_NONE = 0,
    ESPGPU_FMT_INDEX8 = 1,   /**< 8 位调色板索引，1 B/px */
    ESPGPU_FMT_RGB565 = 2,   /**< 2 B/px */
    ESPGPU_FMT_RGBA4444 = 3, /**< 2 B/px */
    ESPGPU_FMT_RGBA8888 = 4, /**< 4 B/px，上传时由 host 降到上面几种 */
    ESPGPU_FMT_R8 = 5,       /**< 单通道，1 B/px（光照贴图 / alpha 遮罩） */
    ESPGPU_FMT_DEPTH16 = 6,  /**< 2 B/px */
};

/** 采样过滤 */
enum {
    ESPGPU_FILTER_NEAREST = 0,
    ESPGPU_FILTER_LINEAR = 1,
};

/** 环绕模式 */
enum {
    ESPGPU_WRAP_REPEAT = 0,
    ESPGPU_WRAP_CLAMP = 1,
    ESPGPU_WRAP_MIRROR = 2,
};

/* ── 管线状态 ─────────────────────────────────────────────────────── */

/**
 * 内置着色模式。设备端不跑通用 shader，而是实现一组固定模式，
 * 由 host 前端负责把 GL 状态（texenv / alphafunc / fog / 光照）折叠成其中之一，
 * 或者把 GLSL 编译成字节码（M6 的 VM 路径）后以 ESPGPU_SHADE_PROGRAM 运行。
 */
enum {
    ESPGPU_SHADE_FLAT = 0,          /**< 纯色（顶点色） */
    ESPGPU_SHADE_TEX = 1,           /**< 纹理 */
    ESPGPU_SHADE_TEX_MOD_COLOR = 2, /**< 纹理 × 顶点色 */
    ESPGPU_SHADE_TEX_MOD_TEX = 3,   /**< 纹理 × 纹理（光照贴图） */
    ESPGPU_SHADE_TEX_ADD = 4,       /**< 纹理 + 顶点色 */
    ESPGPU_SHADE_TEX_ADD_TEX = 5,   /**< 纹理 + 纹理 */
    ESPGPU_SHADE_TEX_MOD_ADD_COLOR = 6, /**< 纹理 × 纹理 + 顶点色 */
    ESPGPU_SHADE_PROGRAM = 7,       /**< 片元 VM 字节码 */
    ESPGPU_SHADE_TEX_DECAL = 8,     /**< rgb = mix(色, 纹理, 纹理.a)，a = 色.a（GL_DECAL） */
};

/*
 * 3D 管线语义（M3 起，PIPELINE + VERTEX_LAYOUT + DRAW(ESPGPU_DRAWF_3D)）——前端必须按此实现：
 *
 * 顶点：整数属性 normalized ? raw/类型最大值 : raw / 2^frac_bits；F32 原值；FIXED16_16 = raw/65536。
 *       缺省属性：COLOR = COLOR 命令的常量色（初值白），TEXCOORDn = (0,0,0,1)，NORMAL = (0,0,1)。
 *       POSITION 2 分量补 z=0 w=1，3 分量补 w=1。
 * 变换：clip = MVP * pos（行主序矩阵，列向量）。透视除法后
 *       屏幕帧缓冲：NDC y=+1 在视口顶端；视口 / 裁剪矩形为左上角原点。
 *       FBO 纹理目标：NDC y=-1 落在第 0 行（与 GL 纹理 t=0 一致），视口 / 裁剪为左下角原点。
 *       深度 = znear + (z_ndc+1)/2*(zfar-znear)，存为 16 位。
 * 绕序：在 NDC（y 向上）里判定，与 GL 相同；front_face=CCW 即 GL_CCW。
 * 纹理坐标：归一化 [0,1]，t=0 是上传数据的第一行（与 glTexImage2D 一致）。
 *       未绑定纹理的单元采样结果为 (1,1,1,1)；R8 = 亮度 (L,L,L,1)；无 alpha 平面时 a=1。
 * 着色（shade_mode，C = 插值顶点色，T0/T1 = 单元 0/1 采样）：
 *       FLAT: C   TEX: T0   TEX_MOD_COLOR: T0*C   TEX_MOD_TEX: T0*T1*C
 *       TEX_ADD: rgb=T0+C a=T0.a*C.a   TEX_ADD_TEX: rgb=T0*C+T1 a=T0.a*C.a
 *       TEX_MOD_ADD_COLOR: rgb=T0*T1+C a=T0.a   TEX_DECAL: rgb=mix(C,T0,T0.a) a=C.a
 *       然后依次：雾（f 由 clip w 计算，即眼空间深度）→ alpha 测试 → 深度测试 → 混合。
 *       帧缓冲没有 alpha 通道，DST_ALPHA 视为 1。
 * 线 / 点：投影后按 line_width / point_size 展成屏幕对齐的四边形。
 * CLEAR 受裁剪矩形约束（GL 语义）。
 */

/** 混合因子（对齐 GL 语义，便于前端 1:1 映射） */
enum {
    ESPGPU_BLEND_ZERO = 0,
    ESPGPU_BLEND_ONE = 1,
    ESPGPU_BLEND_SRC_COLOR = 2,
    ESPGPU_BLEND_ONE_MINUS_SRC_COLOR = 3,
    ESPGPU_BLEND_DST_COLOR = 4,
    ESPGPU_BLEND_ONE_MINUS_DST_COLOR = 5,
    ESPGPU_BLEND_SRC_ALPHA = 6,
    ESPGPU_BLEND_ONE_MINUS_SRC_ALPHA = 7,
    ESPGPU_BLEND_DST_ALPHA = 8,
    ESPGPU_BLEND_ONE_MINUS_DST_ALPHA = 9,
};

/** 比较函数（对齐 GL 语义） */
enum {
    ESPGPU_CMP_NEVER = 0,
    ESPGPU_CMP_LESS = 1,
    ESPGPU_CMP_EQUAL = 2,
    ESPGPU_CMP_LEQUAL = 3,
    ESPGPU_CMP_GREATER = 4,
    ESPGPU_CMP_NOTEQUAL = 5,
    ESPGPU_CMP_GEQUAL = 6,
    ESPGPU_CMP_ALWAYS = 7,
};

/** 管线状态位掩码 —— 用于增量更新，只改变化的部分 */
enum {
    ESPGPU_PS_BLEND = 1u << 0,
    ESPGPU_PS_DEPTH = 1u << 1,
    ESPGPU_PS_ALPHA_TEST = 1u << 2,
    ESPGPU_PS_CULL = 1u << 3,
    ESPGPU_PS_SHADE = 1u << 4,
    ESPGPU_PS_FOG = 1u << 5,
    ESPGPU_PS_SCISSOR = 1u << 6,
    ESPGPU_PS_TEXTURE = 1u << 7,
};

typedef struct {
    uint16_t mask; /**< ESPGPU_PS_* 位掩码，未置位的字段保持原值 */

    /* 混合 */
    uint8_t blend_enable;
    uint8_t blend_src;
    uint8_t blend_dst;
    uint8_t blend_op; /**< 0=add 1=sub 2=rev_sub 3=min 4=max */

    /* 深度 */
    uint8_t depth_test;
    uint8_t depth_write;
    uint8_t depth_func;

    /* Alpha 测试 */
    uint8_t alpha_test;
    uint8_t alpha_func;
    uint8_t alpha_ref;

    /* 剔除 */
    uint8_t cull_mode; /**< 0=off 1=front 2=back */
    uint8_t front_face;/**< 0=cw 1=ccw */

    /* 着色 */
    uint8_t shade_mode; /**< ESPGPU_SHADE_* */
    uint8_t blend_shade;/**< 片元 VM 的混合方式，PROGRAM 模式使用 */

    /* 雾 */
    uint8_t fog_enable;
    uint8_t fog_mode;   /**< 0=linear 1=exp 2=exp2 */
    uint16_t fog_color; /**< RGB565 */
    /* 紧随其后：float fog_start, fog_end, fog_density（设备端用定点处理） */

    /* 纹理单元采样状态 */
    uint8_t tex_count;
    uint8_t tex_unit[ESPGPU_MAX_TEXTURE_UNITS];
    uint16_t tex_id[ESPGPU_MAX_TEXTURE_UNITS];
    uint8_t tex_filter[ESPGPU_MAX_TEXTURE_UNITS];
    uint8_t tex_wrap_u[ESPGPU_MAX_TEXTURE_UNITS];
    uint8_t tex_wrap_v[ESPGPU_MAX_TEXTURE_UNITS];
} espgpu_pipeline_t;

/**
 * PIPELINE 命令体 = espgpu_pipeline_t
 *                 [+ espgpu_pipeline_fog_t]   （命令体长度够才解析）
 *                 [+ espgpu_pipeline_ext_t]
 * 追加字段按长度判断是否存在，旧主机发的短命令体仍然合法。
 */
typedef struct {
    float fog_start;
    float fog_end;
    float fog_density;
} espgpu_pipeline_fog_t;

/** 着色插值 */
enum {
    ESPGPU_SHADE_MODEL_FLAT = 0,   /**< 取激发顶点（GL：最后一个顶点）的颜色 */
    ESPGPU_SHADE_MODEL_SMOOTH = 1, /**< 高洛德：顶点色透视校正插值 */
};

/** 纹理单元 1 的组合方式（多纹理时 shade_mode 用 TEX_MOD_TEX 等） */
typedef struct {
    uint8_t shade_model;  /**< ESPGPU_SHADE_MODEL_* */
    uint8_t color_write;  /**< 0 = 不写颜色（仅深度），非 0 = 写 */
    uint8_t program_id;   /**< shade_mode == PROGRAM 时使用的片元程序 */
    uint8_t tex_matrix;   /**< bit0：对 texcoord0 应用 TEXTURE0 矩阵；bit1：texcoord1 / TEXTURE1 */
    uint8_t lighting;     /**< 1 = 顶点光照（需要 NORMAL 属性 + LIGHTING 命令） */
    uint8_t depth_bias;   /**< glPolygonOffset 的近似：深度减去 depth_bias（16 位单位） */
    uint8_t line_width;   /**< 线宽像素，0 视为 1 */
    uint8_t point_size;   /**< 点大小像素，0 视为 1 */
} espgpu_pipeline_ext_t;

enum {
    ESPGPU_BLEND_OP_ADD = 0,
    ESPGPU_BLEND_OP_SUB = 1,
    ESPGPU_BLEND_OP_REV_SUB = 2,
    ESPGPU_BLEND_OP_MIN = 3,
    ESPGPU_BLEND_OP_MAX = 4,
};

enum {
    ESPGPU_FOG_LINEAR = 0,
    ESPGPU_FOG_EXP = 1,
    ESPGPU_FOG_EXP2 = 2,
};

enum {
    ESPGPU_CULL_NONE = 0,
    ESPGPU_CULL_FRONT = 1,
    ESPGPU_CULL_BACK = 2,
    ESPGPU_CULL_BOTH = 3,
};

enum {
    ESPGPU_FRONT_CW = 0,
    ESPGPU_FRONT_CCW = 1,
};

/** 矩阵槽位 */
enum {
    ESPGPU_MATRIX_MVP = 0,    /**< model * view * projection，设备端用它做顶点变换 */
    ESPGPU_MATRIX_MODELVIEW = 1, /**< 供光照计算用 */
    ESPGPU_MATRIX_PROJECTION = 2,
    ESPGPU_MATRIX_TEXTURE0 = 3, /**< 纹理坐标 0 的矩阵（glMatrixMode(GL_TEXTURE)） */
    ESPGPU_MATRIX_NORMAL = 4,   /**< 法线矩阵（modelview 左上 3x3 的逆转置），光照用 */
    ESPGPU_MATRIX_TEXTURE1 = 5,
    ESPGPU_MATRIX_COUNT = 6,
};

enum {
    ESPGPU_MATF_TRANSPOSE = 1u << 0,
    ESPGPU_MATF_FLOAT = 1u << 1, /**< 载荷是 16 个 float32 而不是 16.16 定点（追加语义） */
    ESPGPU_MATF_IDENTITY = 1u << 2, /**< 无载荷，置为单位阵 */
};

/**
 * 矩阵约定：行主序 m[row*4+col]，列向量右乘：clip = M * (x,y,z,w)^T。
 * GL 前端传的 column-major 矩阵要么转置后发，要么带 ESPGPU_MATF_TRANSPOSE。
 */
typedef struct {
    uint8_t slot;    /**< ESPGPU_MATRIX_* */
    uint8_t flags;   /**< ESPGPU_MATF_* */
    uint16_t pad;
    /* 紧随其后：16 个 int32（定点 16.16）或 16 个 float32，行主序 */
} espgpu_set_matrix_t;

/** 顶点属性类型 */
enum {
    ESPGPU_TYPE_I8 = 0,
    ESPGPU_TYPE_U8 = 1,
    ESPGPU_TYPE_I16 = 2,
    ESPGPU_TYPE_U16 = 3,
    ESPGPU_TYPE_I32 = 4,
    ESPGPU_TYPE_U32 = 5,
    ESPGPU_TYPE_F32 = 6,
    ESPGPU_TYPE_FIXED16_16 = 7, /**< 定点 16.16，带宽最省 */
    ESPGPU_TYPE_RGB565 = 8,     /**< 打包颜色，components 必须为 1，alpha = 1 */
    ESPGPU_TYPE_RGBA4444 = 9,   /**< 打包颜色，components 必须为 1 */
};

/** 属性语义槽 */
enum {
    ESPGPU_ATTR_POSITION = 0,  /**< 2 或 3 分量 */
    ESPGPU_ATTR_NORMAL = 1,
    ESPGPU_ATTR_COLOR = 2,     /**< RGB565 打包，或 4 分量 */
    ESPGPU_ATTR_TEXCOORD0 = 3,
    ESPGPU_ATTR_TEXCOORD1 = 4,
    ESPGPU_ATTR_TEXCOORD2 = 5,
    ESPGPU_ATTR_TEXCOORD3 = 6,
};

typedef struct {
    uint8_t slot;       /**< ESPGPU_ATTR_* */
    uint8_t type;       /**< ESPGPU_TYPE_* */
    uint8_t components; /**< 1..4 */
    uint8_t normalized; /**< 整数类型：1 = 按类型最大值归一化到 [0,1] / [-1,1] */
    uint16_t offset;    /**< 结构体内偏移 */
    uint8_t frac_bits;  /**< 整数类型且未归一化时的小数位数：值 = raw / 2^frac_bits（原 pad，0 = 整数） */
    uint8_t pad;
} espgpu_vertex_attrib_t;

typedef struct {
    uint16_t stride;
    uint8_t attrib_count;
    uint8_t index_type; /**< 0=不索引 1=u16 2=u32 */
    espgpu_vertex_attrib_t attribs[ESPGPU_MAX_VERTEX_ATTRIBS];
} espgpu_vertex_layout_t;

/** 图元拓扑 */
enum {
    ESPGPU_PRIM_POINTS = 0,
    ESPGPU_PRIM_LINES = 1,
    ESPGPU_PRIM_LINE_STRIP = 2,
    ESPGPU_PRIM_TRIANGLES = 3,
    ESPGPU_PRIM_TRIANGLE_STRIP = 4,
    ESPGPU_PRIM_TRIANGLE_FAN = 5,
    ESPGPU_PRIM_QUADS = 6, /**< 原生四边形，避免 host 端拆成三角形 */
    ESPGPU_PRIM_LINE_LOOP = 7,
};

/** 顶点数据来源 */
enum {
    /** 顶点数据内联在命令体里，紧随 espgpu_draw_t */
    ESPGPU_VSRC_INLINE = 0,
    /** 从设备端缓冲区取，stream_id 由 DATA chunk 建立 */
    ESPGPU_VSRC_BUFFER = 1,
};

/** 混合模式（M2 的简化集合，语义对齐 GL） */
enum {
    ESPGPU_BLEND_NONE = 0,   /**< 直接覆盖 */
    ESPGPU_BLEND_ALPHA = 1,  /**< src*a + dst*(1-a) */
    ESPGPU_BLEND_ADD = 2,    /**< src + dst */
    ESPGPU_BLEND_MUL = 3,    /**< src * dst */
};

/**
 * M2 的 2D 顶点：屏幕坐标 + 纹理坐标 + 顶点色。
 *
 * 坐标为 int16 定点：
 *   x, y  —— 1/16 像素，范围 ±2048 像素，够 800x600 用
 *   u, v  —— 1/16 texel
 * 8 字节/顶点，4 顶点/四边形 = 32 字节/四边形。
 */
typedef struct {
    int16_t x, y;
    int16_t u, v;
    uint16_t color; /**< RGB565 顶点色 */
} espgpu_vertex2d_t;

typedef struct {
    uint8_t prim;      /**< ESPGPU_PRIM_* */
    uint8_t source;    /**< ESPGPU_VSRC_* */
    uint16_t vertex_count;
    uint16_t index_count; /**< 0 = 非索引绘制 */
    uint16_t base_vertex;
    uint32_t buffer_id;   /**< VSRC_BUFFER 时的流 ID */
    uint32_t index_offset;/**< 索引在流内的字节偏移 */
} espgpu_draw_t;

typedef struct {
    uint16_t x, y, w, h;
} espgpu_rect_t;

typedef struct {
    uint8_t flags;      /**< bit0 清颜色 bit1 清深度 bit2 清模板 */
    uint8_t pad;
    uint16_t color;     /**< RGB565 */
    uint32_t depth;     /**< 深度清除值（16 位有效） */
} espgpu_clear_t;

/* ── M2：2D 绘制命令 ──────────────────────────────────────────────── */

/** M2 的管线子集。字段是追加式的，后续阶段只会在后面加字段。 */
typedef struct {
    uint8_t blend;      /**< ESPGPU_BLEND_* */
    uint8_t filter;     /**< ESPGPU_FILTER_* */
    uint8_t wrap;       /**< ESPGPU_WRAP_* */
    uint8_t alpha_test;
    uint8_t alpha_ref;
    uint8_t pad[3];
} espgpu_pipeline2d_t;

typedef struct {
    uint16_t unit;
    uint16_t texture;
    uint8_t filter;
    uint8_t wrap;
    uint16_t pad;
} espgpu_bind_texture_t;

/**
 * 纹理上传。一张纹理可拆成多条命令：
 *   offset == 0 的那条负责分配并按 total_len 校验，后续条按 offset 续写。
 */
typedef struct {
    uint32_t id;
    uint16_t width;
    uint16_t height;
    uint32_t offset;    /**< 本分片在纹理内的字节偏移 */
    uint32_t total_len; /**< 整张纹理的字节数 */
    uint8_t format;     /**< ESPGPU_FMT_RGB565 / RGBA4444 / INDEX8 / R8 */
    uint8_t filter;
    uint8_t wrap;
    uint8_t pad;
} espgpu_upload_texture_t;

/* ── M2-M6 追加命令体 ─────────────────────────────────────────────── */

/**
 * TEX_PALETTE：INDEX8 纹理的调色板。
 * 命令体 = 本结构 + count × u16 RGB565 + count × u8 alpha。
 * 可以先于或晚于像素上传；未设置时用灰阶、alpha=255。
 */
typedef struct {
    uint32_t id;
    uint16_t first;
    uint16_t count;
} espgpu_tex_palette_t;

/** DELETE_TEXTURE。id == ESPGPU_TEXTURE_ALL 时清空整个纹理缓存。 */
typedef struct {
    uint32_t id;
} espgpu_delete_texture_t;

#define ESPGPU_TEXTURE_ALL 0xFFFFFFFFu

/** COLOR：常量顶点色，RGBA8888。 */
typedef struct {
    uint8_t r, g, b, a;
} espgpu_color_t;

#define ESPGPU_MAX_LIGHTS 4u

/** LIGHTING 里的一盏灯。方向在眼空间、已归一化、指向光源（GL 的 w=0 灯）。 */
typedef struct {
    float dir[3];
    float diffuse[3];
} espgpu_light_t;

/**
 * LIGHTING：命令体 = 本结构 + count × espgpu_light_t。
 * 颜色 = clamp(ambient + Σ max(0, N·L) × diffuse) × 顶点色（等价 GL_COLOR_MATERIAL 的
 * AMBIENT_AND_DIFFUSE），alpha 取顶点 alpha。
 */
typedef struct {
    uint8_t enable;
    uint8_t count;
    uint16_t pad;
    float ambient[3];
} espgpu_lighting_t;

/** LOAD_PROGRAM：命令体 = 本结构 + insn_count × espgpu_vm_insn_t（见 espgpu_vm.h）。 */
typedef struct {
    uint8_t program_id;
    uint8_t varying_count; /**< 使用的 v 寄存器个数 */
    uint16_t insn_count;
} espgpu_load_program_t;

/** SET_UNIFORMS：命令体 = 本结构 + count × 4 × float32，写进 c[first..first+count)。 */
typedef struct {
    uint8_t first;
    uint8_t count;
    uint16_t pad;
} espgpu_set_uniforms_t;

typedef struct {
    float znear; /**< [0,1] */
    float zfar;
} espgpu_depth_range_t;

/** VIEWPORT 命令体 = espgpu_rect_t（左上角原点、y 向下，与帧缓冲行序一致）。 */

/** FBO 动作 */
enum {
    ESPGPU_FBO_BIND_TEXTURE = 0, /**< 渲染到纹理 texture_id（不存在则按 width/height 新建 RGB565） */
    ESPGPU_FBO_BIND_DEFAULT = 1, /**< 回到屏幕帧缓冲 */
    ESPGPU_FBO_COPY_TO_TEXTURE = 2, /**< 把当前目标的 (x,y,w,h) 拷进纹理（glCopyTexImage2D） */
};

typedef struct {
    uint8_t action;  /**< ESPGPU_FBO_* */
    uint8_t pad;
    uint16_t pad2;
    uint32_t texture_id;
    uint16_t width, height;
    uint16_t x, y;   /**< COPY_TO_TEXTURE 的源矩形 */
} espgpu_fbo_t;

/** FENCE：执行到这里时设备回 EVENT{kind=FENCE, arg=value}。 */
typedef struct {
    uint32_t value;
} espgpu_fence_t;

/**
 * DATA chunk 载荷头。设备端缓冲（顶点 / 索引）用于显示列表与静态几何，
 * 一个缓冲可以跨多个 DATA chunk 续写：offset == 0 的那片负责（重新）分配。
 */
typedef struct {
    uint32_t buffer_id;
    uint32_t offset;
    uint32_t total_len;
    uint8_t kind;   /**< ESPGPU_DATA_* */
    uint8_t flags;  /**< ESPGPU_DATAF_* */
    uint16_t pad;
} espgpu_data_hdr_t;

enum {
    ESPGPU_DATA_BUFFER = 0,
};

enum {
    ESPGPU_DATAF_DELETE = 1u << 0, /**< 释放 buffer_id，忽略数据 */
};

#define ESPGPU_MAX_BUFFERS 256u

/* ════════════════════════════════════════════════════════════════════
 * 3. Frame 层（D2H）
 * ════════════════════════════════════════════════════════════════════ */

/** 帧编码方式 */
enum {
    ESPGPU_CODEC_RAW = 0,        /**< 裸数据 */
    ESPGPU_CODEC_RLE = 1,        /**< 逐行 RLE */
    ESPGPU_CODEC_DELTA_RLE = 2,  /**< 与上一帧异或后 RLE */
    ESPGPU_CODEC_TILE_DELTA = 3, /**< tile 脏标记 + 每 tile 内 RLE */
};

/** 帧分片标志 */
enum {
    ESPGPU_FF_FIRST = 1u << 0,
    ESPGPU_FF_LAST = 1u << 1,
    ESPGPU_FF_DROPPED = 1u << 2, /**< 本帧因背压被标记为不连续 */
    ESPGPU_FF_VSYNC = 1u << 3,
};

/**
 * 帧头。同一帧的所有分片共享相同的 frame_id；只有第一片带完整头，
 * 后续分片用 ESPGPU_FF_FIRST 判断，头仍然重复以保证单包可解析。
 */
typedef struct {
    uint32_t frame_id;
    uint16_t width;
    uint16_t height;
    uint16_t origin_x;
    uint16_t origin_y;
    uint16_t origin_w;
    uint16_t origin_h;
    uint32_t data_len;   /**< 本分片内编码数据字节数 */
    uint32_t raw_len;    /**< 整帧裸数据字节数 */
    uint32_t total_enc;  /**< 整帧编码后字节数，主机用它校验分片是否有缺失 */
    uint8_t format;      /**< ESPGPU_FMT_* */
    uint8_t codec;       /**< ESPGPU_CODEC_* */
    uint8_t frag_index;
    uint8_t frag_count;
    uint8_t flags;       /**< ESPGPU_FF_* */
    uint8_t palette_dirty; /**< 1 = 本帧前附带 256 项 CLUT */
} espgpu_frame_hdr_t;

#define ESPGPU_CLUT_ENTRIES 256u

/* ════════════════════════════════════════════════════════════════════
 * 4. Control 层
 * ════════════════════════════════════════════════════════════════════ */

enum {
    ESPGPU_CTRL_PING = 0x0001,
    ESPGPU_CTRL_GET_DEVICE_INFO = 0x0002,
    ESPGPU_CTRL_GET_STATS = 0x0003,
    ESPGPU_CTRL_SET_MODE = 0x0004,
    ESPGPU_CTRL_SET_CLUT = 0x0005,
    ESPGPU_CTRL_RESET_STATS = 0x0006,
    ESPGPU_CTRL_SET_CLOCK = 0x0007,
    ESPGPU_CTRL_OTA_BEGIN = 0x0010,
    ESPGPU_CTRL_OTA_DATA = 0x0011,
    ESPGPU_CTRL_OTA_END = 0x0012,
    ESPGPU_CTRL_OTA_ABORT = 0x0013,
    ESPGPU_CTRL_LOG_SUBSCRIBE = 0x0020,
    ESPGPU_CTRL_SOFT_RESET = 0x0030,
    ESPGPU_CTRL_REQUEST_KEYFRAME = 0x0031, /**< 主机解码失败时要求下一帧走非差分 */
    ESPGPU_CTRL_SET_DEBUG_VIEW = 0x0040,   /**< arg0 = ESPGPU_DEBUG_VIEW_* */
    ESPGPU_CTRL_SET_CODEC = 0x0041,        /**< arg0 = 命令模式下 PRESENT 使用的编码 */
};

/** 调试视图：替换 PRESENT 时回传的内容，不影响渲染本身 */
enum {
    ESPGPU_DEBUG_VIEW_NONE = 0,
    ESPGPU_DEBUG_VIEW_DEPTH = 1,    /**< 深度缓冲灰度 */
    ESPGPU_DEBUG_VIEW_OVERDRAW = 2, /**< 每像素片元数热力图 */
    ESPGPU_DEBUG_VIEW_TILES = 3,    /**< 每 tile 三角形数热力图 + tile 边框 */
    ESPGPU_DEBUG_VIEW_WIREFRAME = 4,/**< 三角形边 */
    ESPGPU_DEBUG_VIEW_COUNT = 5,
};

/** 状态码 */
enum {
    ESPGPU_OK = 0,
    ESPGPU_ERR_INVALID_ARG = 1,
    ESPGPU_ERR_UNSUPPORTED = 2,
    ESPGPU_ERR_NO_MEM = 3,
    ESPGPU_ERR_BAD_STATE = 4,
    ESPGPU_ERR_BAD_MAGIC = 5,
    ESPGPU_ERR_CRC = 6,
    ESPGPU_ERR_OVERFLOW = 7,
    ESPGPU_ERR_TIMEOUT = 8,
    ESPGPU_ERR_INTERNAL = 9,
};

typedef struct {
    uint16_t op;     /**< ESPGPU_CTRL_* */
    uint16_t seq;    /**< 请求序号，响应原样带回 */
    uint32_t arg0;
    uint32_t arg1;
} espgpu_ctrl_req_t;

typedef struct {
    uint16_t op;
    uint16_t seq;
    uint16_t status; /**< ESPGPU_OK / ESPGPU_ERR_* */
    uint16_t len;    /**< 后续数据字节数 */
} espgpu_ctrl_resp_t;

/** SET_MODE 参数 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t format;   /**< ESPGPU_FMT_RGB565 / INDEX8 */
    uint8_t codec;    /**< ESPGPU_CODEC_* */
    uint8_t max_fps;
    uint8_t vsync;
} espgpu_mode_t;

/**
 * M1 测试图案 ID，作为 SET_MODE 的 arg0 传给设备。
 * M2 接入光栅器后这些值会被真实渲染场景取代，但枚举本身保留，
 * 用于链路自检和压力测试。
 */
enum {
    ESPGPU_PATTERN_BARS = 0,    /**< 移动色条：平坦、高压缩率 */
    ESPGPU_PATTERN_UI = 1,      /**< 面板 + 细网格 + 移动方块 */
    ESPGPU_PATTERN_DOOMISH = 2, /**< 天花板/地板渐变 + 竖条墙面 */
    ESPGPU_PATTERN_NOISE = 3,   /**< 伪随机噪声：最坏情况 */
    ESPGPU_PATTERN_MIX = 4,     /**< 上半平坦 + 下半噪声 */
    ESPGPU_PATTERN_COUNT = 5,
};

/** 支持的编码方式位掩码 */
enum {
    ESPGPU_CAP_CODEC_RAW = 1u << ESPGPU_CODEC_RAW,
    ESPGPU_CAP_CODEC_RLE = 1u << ESPGPU_CODEC_RLE,
    ESPGPU_CAP_CODEC_DELTA_RLE = 1u << ESPGPU_CODEC_DELTA_RLE,
    ESPGPU_CAP_CODEC_TILE_DELTA = 1u << ESPGPU_CODEC_TILE_DELTA,
};

/** 能力位 */
enum {
    ESPGPU_CAP_INDEX8 = 1u << 0,
    ESPGPU_CAP_RGB565 = 1u << 1,
    ESPGPU_CAP_DEPTH = 1u << 2,
    ESPGPU_CAP_FBO = 1u << 3,
    ESPGPU_CAP_FRAGMENT_VM = 1u << 4,
    ESPGPU_CAP_QUADS = 1u << 5,
    ESPGPU_CAP_LIGHTING = 1u << 6,
    ESPGPU_CAP_OTA = 1u << 7,
    ESPGPU_CAP_TNL = 1u << 8,          /**< VERTEX_LAYOUT + 3D DRAW + 矩阵 */
    ESPGPU_CAP_TEX_LRU = 1u << 9,
    ESPGPU_CAP_PALETTED_TEX = 1u << 10,
    ESPGPU_CAP_BUFFERS = 1u << 11,     /**< DATA chunk 设备端缓冲 */
    ESPGPU_CAP_CLOCK = 1u << 12,       /**< SET_CLOCK */
    ESPGPU_CAP_DEBUG_VIEW = 1u << 13,
    ESPGPU_CAP_LZ = 1u << 14,          /**< 接受 ESPGPU_F_LZ 压缩的下行 chunk */
    ESPGPU_CAP_LOG = 1u << 15,         /**< LOG_SUBSCRIBE */
};

typedef struct {
    uint16_t proto_version;
    uint8_t fw_major, fw_minor, fw_patch;
    uint32_t fw_git;
    char chip_model[24];
    uint16_t max_width, max_height;
    uint16_t min_width, min_height;
    uint8_t psram_mb, flash_mb;
    uint16_t reserved0;
    uint32_t psram_total, psram_free;
    uint32_t sram_total, sram_free;
    uint32_t flash_total, flash_used;
    uint16_t capabilities;   /**< ESPGPU_CAP_* */
    uint16_t codecs;         /**< ESPGPU_CAP_CODEC_* */
    uint32_t texture_cache_size;
    uint32_t max_texture_dim;
    uint16_t max_texture_units;
    uint16_t max_vertex_attribs;
    uint8_t cpu_freq_mhz;
    uint8_t psram_freq_mhz;
    uint16_t reserved1;
} espgpu_device_info_t;

typedef struct {
    uint32_t uptime_ms;
    uint32_t host_seq_seen;
    uint32_t host_errors;
    uint32_t frames_presented;
    uint32_t frames_dropped;
    uint32_t bytes_in;     /**< 从 host 收到的字节 */
    uint32_t bytes_out;    /**< 发给 host 的字节 */
    uint32_t cmd_count;
    uint32_t draw_calls;
    uint32_t primitives;
    uint32_t vertices;
    uint32_t fill_pixels;
    uint32_t tiles_dirty;
    uint32_t frame_raw_bytes;
    uint32_t frame_enc_bytes;
    uint32_t t_encode_us;  /**< 上一帧编码耗时 */
    uint32_t t_raster_us;  /**< 上一帧光栅化耗时 */
    uint32_t t_usb_us;     /**< 上一帧 USB 发送耗时 */
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t free_psram;
    int16_t internal_temp_c10; /**< 内部温度 ×10，未知为 INT16_MIN */
    int16_t psram_temp_c10;
    uint32_t crc_errors;
    uint32_t seq_gaps;
    /* ── 以下为 M2-M6 追加；主机收到较短的响应时把缺失部分视为 0 ── */
    uint32_t tex_resident;    /**< 当前驻留纹理数 */
    uint32_t tex_bytes;       /**< 纹理缓存占用 */
    uint32_t tex_evictions;   /**< 累计 LRU 换出 */
    uint32_t tex_misses;      /**< 绘制时纹理不在缓存的次数 */
    uint32_t triangles;       /**< 上一帧送入光栅的三角形（裁剪/剔除后） */
    uint32_t culled;          /**< 上一帧被剔除的三角形 */
    uint32_t bin_bytes;       /**< 上一帧 tile 分箱占用 */
    uint32_t t_tnl_us;        /**< 上一帧 T&L + 分箱耗时 */
    uint16_t cpu_freq_mhz;
    uint8_t debug_view;
    uint8_t ota_state;        /**< 0 空闲 1 写入中 2 待重启 3 失败 */
    uint32_t ota_written;
    uint32_t buffers_bytes;
    uint32_t lz_in_bytes;     /**< 累计压缩的下行字节（解压前） */
    uint32_t lz_out_bytes;    /**< 解压后 */
} espgpu_stats_t;

/** EVENT 类型 */
enum {
    ESPGPU_EVT_VSYNC = 1,
    ESPGPU_EVT_FENCE = 2,
    ESPGPU_EVT_ERROR = 3,
    ESPGPU_EVT_MODE_CHANGED = 4,
    ESPGPU_EVT_OTA_PROGRESS = 5,
    ESPGPU_EVT_TEXTURE_EVICTED = 6, /**< arg = 被设备 LRU 换出的纹理 id，主机下次绑定前需重传 */
    ESPGPU_EVT_BUFFER_EVICTED = 7,  /**< arg = 因内存不足被丢弃的缓冲 id */
};

typedef struct {
    uint8_t kind;   /**< ESPGPU_EVT_* */
    uint8_t code;
    uint16_t value;
    uint32_t arg;
} espgpu_event_t;

/* ════════════════════════════════════════════════════════════════════
 * 5. CRC
 * ════════════════════════════════════════════════════════════════════ */

/** CRC16-CCITT (0x1021, init 0xFFFF)，半字节查表，XIP 下也够快。 */
static inline uint16_t espgpu_crc16(uint16_t crc, const void *data, uint32_t len)
{
    static const uint16_t tbl[16] = {
        0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
        0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    };
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        crc = (uint16_t)((crc << 4) ^ tbl[((crc >> 12) ^ (*p >> 4)) & 0x0F]);
        crc = (uint16_t)((crc << 4) ^ tbl[((crc >> 12) ^ (*p & 0x0F)) & 0x0F]);
        p++;
    }
    return crc;
}

/** 计算一个完整 Chunk 的 CRC（header + payload，不含 CRC 自身）。 */
static inline uint16_t espgpu_chunk_crc(const espgpu_chunk_hdr_t *hdr, const void *payload)
{
    uint16_t crc = espgpu_crc16(0xFFFF, hdr, ESPGPU_CHUNK_HDR_SIZE);
    if (hdr->length) {
        crc = espgpu_crc16(crc, payload, hdr->length);
    }
    return crc;
}

#pragma pack(pop)

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ── 编译期自检：这些结构必须精确匹配线上格式 ────────────────────── */
#if defined(__cplusplus)
#define ESPGPU_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define ESPGPU_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

ESPGPU_STATIC_ASSERT(sizeof(espgpu_chunk_hdr_t) == 12, "chunk header must be 12 bytes");
ESPGPU_STATIC_ASSERT(sizeof(espgpu_cmd_t) == 4, "cmd header must be 4 bytes");
ESPGPU_STATIC_ASSERT(sizeof(espgpu_ctrl_req_t) == 12, "ctrl req must be 12 bytes");
ESPGPU_STATIC_ASSERT(sizeof(espgpu_ctrl_resp_t) == 8, "ctrl resp must be 8 bytes");
