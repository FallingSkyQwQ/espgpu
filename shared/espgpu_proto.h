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
};

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

/** 矩阵槽位 */
enum {
    ESPGPU_MATRIX_MVP = 0,    /**< model * view * projection，设备端用它做顶点变换 */
    ESPGPU_MATRIX_MODELVIEW = 1, /**< 供光照计算用 */
    ESPGPU_MATRIX_PROJECTION = 2,
};

typedef struct {
    uint8_t slot;    /**< ESPGPU_MATRIX_* */
    uint8_t flags;   /**< bit0: 转置 */
    uint16_t pad;
    /* 紧随其后：16 个 int32，定点 16.16，行主序 */
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
    uint8_t normalized;
    uint16_t offset;    /**< 结构体内偏移 */
    uint16_t pad;
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
    uint8_t format;     /**< ESPGPU_FMT_RGB565 / RGBA4444 */
    uint8_t filter;
    uint8_t wrap;
    uint8_t pad;
} espgpu_upload_texture_t;

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
} espgpu_stats_t;

/** EVENT 类型 */
enum {
    ESPGPU_EVT_VSYNC = 1,
    ESPGPU_EVT_FENCE = 2,
    ESPGPU_EVT_ERROR = 3,
    ESPGPU_EVT_MODE_CHANGED = 4,
    ESPGPU_EVT_OTA_PROGRESS = 5,
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
