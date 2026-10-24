# ESP32-S3-GPU 项目方案

> 状态：需求已锁定，等待进入实施
> 最后更新：2026-10-24
> 工作目录：`C:\Users\youzilm\Documents\projects\espgpu`

---

## 1. 项目目标

把一块 ESP32-S3-WROOM-1-N16R8 做成一块**真正意义上的 GPU**：Windows 程序通过标准 OpenGL 接口（GL 1.2 固定管线 + GLES 3.0 子集）提交绘制命令，ESP32-S3 完成顶点变换与光栅化，渲染结果通过 USB 回传到 PC 显示。

**成功标准**

1. DOOM2 MAP01 在 prboom-plus（DSDA-Doom 分支）上可正常游玩：能移动、能开枪、精灵/透明/光照正确，**≥ 24fps 稳帧**。
2. 除 Doom 外，能跑通自研的 GL 1.2 / GLES 3.0 程序（通用 GL 兼容性）。
3. 提供一个 NVIDIA 控制面板式的 ImGui 控制台，含帧查看器、性能 HUD、调试视图、OTA、时钟调节、日志。

**明确不做的事**

- 不做 WDDM 内核 miniport / UMD，不做 PCIe，不做 WARP 级软件 D3D 驱动（投入产出比极差，且有蓝屏风险）。
- 不做驱动签名，不做内核态组件，全链路用户态 + WinUSB 免驱。
- 不做音频回传（见 §11 待定项）。
- 不做 shader model 3.0 以上的可编程能力。

---

## 2. 硬件约束（实测口径）

| 项 | 参数 | 影响 |
|---|---|---|
| CPU | Xtensa LX7 双核 @240MHz，单精度 FPU + int8/int16 SIMD | 光栅化/采样可手写 SIMD 加速 |
| 内部 SRAM | 512KB | tile 缓冲与 IRAM 热代码的唯一去处 |
| PSRAM | 8MB Octal @80MHz，实际带宽 ~40–80MB/s | 随机访问慢，禁止光栅器直写 |
| USB | OTG **Full-Speed 12Mbps**（无 HS） | 实测 bulk **0.85–1.0 MB/s**，全项目唯一硬瓶颈 |
| Flash | 16MB | 双分区 OTA + 纹理/字库预留 |

### 带宽账本（核心约束）

取保守值 **0.85 MB/s**：

| 目标帧率 | 每帧总预算（下行命令 + 上行帧） |
|---|---|
| 30fps | 28 KB |
| **24fps** | **35 KB** |
| 20fps | 42 KB |

320×200 一帧的实际数据量：

| 方向 | 裸数据 | 压缩后 |
|---|---|---|
| 下行：~600 四边形 × 48B | ~29 KB | 12–18 KB（heatshrink） |
| 上行：320×200 8-bit 帧 | 64 KB | 6–15 KB（tile 差分 + RLE） |
| **合计** | | **18–33 KB** |

**结论**：24fps 有约 25% 余量，30fps 踩在极限上。默认锁定 24fps，提供运行时降级旋钮。

### 为什么用 320×200 而不是 320×240

Doom 原生宽高比是 8:5（320×200），不是 4:3。用 320×200 白省 17% 带宽，且无需拉伸。

---

## 3. 架构总览

### 设计原则

**两端 GL 共用一个"设备核心"，GL 1.2 与 GLES 3.0 只是两个前端。** 设备接口设计成类似 Vulkan 的显式模型（PipelineState + Uniform + VertexStream + Draw + Present），这样协议只需设计一次，也更接近真实 GPU 的结构。

```
┌ Windows ────────────────────────────────────────────────────┐
│ DSDA-Doom (fork)            自研 GLES 程序                   │
│      │ GL 1.2 固定管线            │ glUseProgram / VAO        │
│      ▼                            ▼                          │
│  opengl32.dll (GL1.2 前端)   libGLESv2.dll (GLES3 前端)      │
│  · 矩阵栈 → 矩阵             · GLSL ES 3.0 编译器           │
│  · texenv → 混合模式         · 模式识别 → 内置模式          │
│  · alphafunc → 模式          · 不识别 → 字节码 VM           │
│      └────────────┬───────────────┘                          │
│                   ▼                                          │
│            espgpu.dll  ← 统一设备接口                        │
│       (PSO / Uniform / VertexStream / Draw / Present)        │
│                   │                                          │
│      WinUSB bulk OUT ──┐         ┌── WinUSB bulk IN          │
│                        │         │                           │
│  espgpu-panel.exe (ImGui, 免签)  │                           │
│   帧查看器 / 性能 HUD / 调试视图 / OTA / 时钟 / 日志          │
└────────────────────────┼─────────┼───────────────────────────┘
                         │ USB FS 12Mbps
┌ ESP32-S3 ──────────────┼─────────┼───────────────────────────┐
│  TinyUSB vendor class（EP1 OUT 命令 / EP2 IN 帧 / EP3 管理） │
│                                                              │
│  Core0: USB 收发(DMA) + 帧压缩(RLE) + 管理通道               │
│  Core1: 命令解析 → T&L(int16 定点) → tile 光栅(SRAM)         │
│                                                              │
│  IRAM: 光栅内层循环、T&L 热路径（必须，XIP 慢 3–5 倍）        │
│  PSRAM: framebuffer / zbuffer / 纹理 LRU / FBO / 命令环      │
└──────────────────────────────────────────────────────────────┘
```

### 三个关键设计决定

**① 全链路 8-bit 调色板**

Doom 天生是 256 色调色板渲染。纹理按 `GL_COLOR_INDEX8` 存（PSRAM 占用 ÷4、上传带宽 ÷4），帧按 8-bit 索引回传（带宽 ÷2，RLE 压缩率暴涨）。PC 端查 CLUT 转 RGB565 上屏。这一条把项目从"勉强可行"变成"有余量"。

**② GPU 端做 T&L**

世界坐标用 int16 定点 + 每次 draw 一个 4×4 矩阵。带宽与"PC 端预变换后发屏幕坐标"完全相同（都是 12B/顶点），但 S3 每帧只需处理约 2400 个顶点（约 0.5ms），几乎零成本。这样才能称为 GPU 而不是像素填充器。

**③ tile 渲染到内部 SRAM，DMA 回写 PSRAM**

64×64 的 tile 用 RGB565 = 8KB，双核各 4 个 tile 共 64KB。**绝不能**让光栅器直接写 PSRAM——会和 USB DMA 抢总线，帧率直接腰斩。

---

## 4. USB 协议

### 端点

| 端点 | 方向 | 用途 |
|---|---|---|
| EP1 OUT | PC → S3 | 命令流（bulk，64B max packet） |
| EP2 IN | S3 → PC | 帧流 + 事件（bulk，64B max packet） |
| EP3 OUT/IN | 双向 | 管理通道：设备信息、统计、OTA、日志（可选独立端点） |

枚举方式：TinyUSB vendor class（class code `0xFF`）+ **Microsoft OS 2.0 Descriptor** 实现 WCID，Windows 自动绑定 WinUSB，**无需 inf、无需签名**。USB product string 设为 `ESP32-S3 GPU`，设备管理器直接显示此名称。

### 命令帧格式

```
[4B 'ESPG'][2B ver][2B seq][2B payload_len][payload][2B crc16]
```

### opcode 集

| 值 | 命令 | 说明 |
|---|---|---|
| `0x01` | `SET_PIPELINE` | blend / depth / alphafunc / cull / fog / 内置着色模式 id |
| `0x02` | `SET_MATRIX` | MVP + 模型矩阵，int32 定点 16.16 |
| `0x03` | `BIND_TEXTURE` | texid + 采样状态（filter / wrap） |
| `0x04` | `UPLOAD_TEXTURE` | 8-bit paletted / RGB565，按 4KB 分块 |
| `0x05` | `DRAW_QUADS` | 原生四边形，顶点数 + 顶点流 + 索引 |
| `0x06` | `DRAW_TRIS` | 3D demo 用 |
| `0x07` | `CLEAR` | 颜色 / 深度 |
| `0x08` | `SWAP` | **帧边界**，触发压缩 + 回传 + 垂直同步 |
| `0x09` | `SET_CLUT` | 256 × RGB565 调色板 |
| `0x0A` | `FBO_BIND` / `FBO_BLIT` | 离屏渲染 |
| `0x0B` | `QUERY` | 统计回读（drawcall / tile / 顶点数 / 填充率） |

### 顶点格式（12 字节，int16 定点）

```
offset 0  pos.xyz   int16 × 3   世界坐标，定点 1/64 world unit
offset 6  uv        int16 × 2   纹理坐标，定点 1/16 texel
offset 10 color     RGB565      顶点色（高洛德）
```

### 帧流格式（S3 → PC）

```
[4B 'ESPF'][2B seq][u8 format][u8 flags][2B len][压缩数据]
```

- `format`: `0` = RGB565，`1` = INDEX8
- `flags`: `b0` 全帧 / `b1` tile 差分 / `b2` 垂直同步事件 / `b3` 帧丢弃（背压信号）

### 背压策略

上行带宽不足时，S3 主动丢弃最旧的待发帧并在 `flags.b3` 标记。PC 端据此统计丢帧率，驱动层按比例降低渲染距离 / 关闭双线性 / 跳过行列，直到恢复 24fps。

---

## 5. 内存布局

### PSRAM 8MB

```
0x000000  framebuffer ×2       INDEX8 128KB / RGB565 256KB
0x040000  z-buffer 16bit       128KB
0x060000  CLUT                 512B
0x061000  texture LRU cache    4.5MB    ← 按需从 WAD 上传，LRU 换出
0x4A0000  command ring         256KB
0x4E0000  FBO targets          512KB
0x560000  heap / stacks        ~1.5MB
```

合计约 6.5MB，余量 1.5MB。

### 内部 SRAM 512KB

```
tile 缓冲        64KB    (8 × 64×64 RGB565)
IRAM 热代码      64KB    (光栅内层循环 + T&L)
USB DMA 缓冲     16KB    (4KB URB × 双缓冲 × 2 方向)
FreeRTOS + 栈    64KB
```

**性能陷阱提示**：光栅器内层循环必须放 IRAM。从 flash XIP 执行慢 3–5 倍，这是 S3 上最容易被忽略的问题。

### 纹理显存管理

Doom 全量纹理按 8-bit 调色板约 8MB，超过缓存容量。采用 **PC 侧权重 + S3 侧 LRU**：

1. 驱动层维护纹理元数据表，记录每张纹理的最近使用帧号。
2. 上传时按需传输，S3 端 LRU 换出最久未用的纹理。
3. 换页粒度 4KB，避免碎片。
4. 提供 `UPLOAD_TEXTURE` 的 `PRIORITY` 标志，供预加载当前关卡的纹理。

---

## 6. API 覆盖范围

### GL 1.2 固定管线（M3 交付）

- 矩阵栈：`glMatrixMode` / `glLoadMatrixf` / `glMultMatrixf` / `glLoadIdentity` / `glPushMatrix` / `glPopMatrix` / `glFrustum` / `glOrtho` / `glRotatef` / `glTranslatef` / `glScalef`
- 顶点数组：`glVertexPointer` / `glTexCoordPointer` / `glColorPointer` / `glEnableClientState` / `glDrawArrays` / `glDrawElements` / `glArrayElement`
- 立即模式：`glBegin` / `glEnd` / `glVertex3f` 等（glboom 部分路径使用，需兼容）
- 纹理：`glGenTextures` / `glBindTexture` / `glTexImage2D` / `glTexSubImage2D` / `glTexParameteri` / `glTexEnvi` / `glDeleteTextures`
- 状态：`glEnable` / `glDisable` / `glBlendFunc` / `glAlphaFunc` / `glDepthFunc` / `glDepthMask` / `glCullFace` / `glFrontFace` / `glShadeModel` / `glHint`
- 雾：`glFogf` / `glFogfv` / `glFogi`
- 其它：`glViewport` / `glScissor` / `glClear` / `glClearColor` / `glClearDepth` / `glFlush` / `glFinish` / `glGetError` / `glGetIntegerv` / `glGetString`

### 必需扩展

| 扩展 | 用途 |
|---|---|
| `GL_ARB_multitexture` | Doom 的「纹理 × 光照」双层 |
| `GL_ARB_texture_env_combine` | glboom 的调制/叠加表达式 |
| `GL_ARB_texture_env_add` | 附加混合模式 |
| `GL_EXT_paletted_texture` | 8-bit 索引纹理直传（带宽关键） |
| `GL_ARB_texture_non_power_of_two` | Doom 纹理尺寸任意 |
| `GL_EXT_texture_filter_anisotropic` | 检测到即禁用（硬件不支持） |

### GLES 3.0 子集（M6 交付）

- 缓冲：VAO / VBO / IBO，`glVertexAttribPointer`，`glDrawElements`
- 程序：`glCreateShader` / `glShaderSource` / `glCompileShader` / `glCreateProgram` / `glLinkProgram` / `glUseProgram`
- Uniform：矩阵、采样器、颜色、`glUniform*`
- 纹理：`RGBA8` / `RGB565` / `R8` / `RGBA4444`，`GL_NEAREST` / `GL_LINEAR` / `GL_CLAMP_TO_EDGE` / `GL_REPEAT`
- 状态：blend / depth / cull / scissor
- FBO：`glGenFramebuffers` / `glFramebufferTexture2D` / `glBindFramebuffer`
- **着色器限制**：PC 端 GLSL ES 3.0 编译器做模式识别，命中内置模式则直接生成 PSO；未命中则编译为字节码在 S3 上解释执行（性能减半，作为兜底）

---

## 7. 着色模式（内置）

S3 端不跑通用 shader，而是实现一组固定模式，由 `SET_PIPELINE` 的 `mode_id` 选择：

| id | 模式 | 用途 |
|---|---|---|
| 0 | `FLAT_COLOR` | 纯色填充 |
| 1 | `TEX_MODULATE` | 纹理 × 顶点色（Doom 默认） |
| 2 | `TEX_MODULATE_LIGHT` | 双纹理调制（纹理 × 光照贴图） |
| 3 | `TEX_ADD` | 纹理 + 顶点色（叠加） |
| 4 | `TEX_ALPHA_BLEND` | 半透明（水、玻璃） |
| 5 | `TEX_ALPHA_TEST` | 透明键（精灵） |
| 6 | `FOG_LINEAR` | 线性雾（可与 1/2 组合） |
| 7 | `DEPTH_ONLY` | 仅写深度（预 pass） |

新增模式只需在 `SET_PIPELINE` 里加 id，不改协议结构。

---

## 8. 里程碑

| # | 内容 | 交付物 | 验收标准 | 估时 |
|---|---|---|---|---|
| **M1** | 链路打通 | ESP-IDF 工程、TinyUSB 描述符 + WCID、`espgpu.dll` 设备层、`espgpu-panel` 骨架（帧查看器/日志/带宽曲线）、测试图案生成器、**压缩率实测工具** | 面板实时显示 320×200 测试图案；上行 ≥ 0.85MB/s；往返延迟 < 5ms；设备管理器免驱识别 | 1.5 周 |
| **M2** | 设备核心 | 命令流协议、tile 光栅器（透视校正/双线性/alpha/Z）、纹理 LRU、8-bit + RGB565 帧编码、CLEAR/DRAW/SWAP | 自研 2D demo：500 drawcall，320×200@30fps 不降级 | 3 周 |
| **M3** | GL 1.2 前端 | `opengl32.dll`（固定管线矩阵栈、texenv、alphafunc、fog、多纹理、paletted texture）、**GL trace 模式 + Mesa llvmpipe 直通模式** | 用 Mesa 直通跑通 DSDA-Doom，导出完整 GL 调用清单 | 2 周 |
| **M4** | Doom | DSDA-Doom fork + espgpu video backend、补齐扩展、输入/帧同步 | **DOOM2 MAP01 可玩，≥24fps 稳帧** | 2.5 周 |
| **M5** | 3D + 面板 | T&L、视锥裁剪、背面剔除、高洛德光照、FBO；面板全功能（调试视图/OTA/时钟/温度） | 自研 3D demo 24fps；面板所有页面可用 | 3 周 |
| **M6** | GLES 3.0 前端 | GLSL ES 3.0 词法/语法/类型检查 → 模式识别 → 内置模式；未识别回退字节码 VM | 标准 GLES 3.0 三角 demo + 一个 shader 移植 demo | 3–4 周 |

**合计 15–16 周**（单人业余节奏约 4–5 个月）。

### 依赖关系

```
M1 ──> M2 ──> M3 ──> M4
        └────> M5 ──> M6
```

M5 可与 M3/M4 部分并行（调试视图和 OTA 不依赖 GL 前端）。

---

## 9. 风险与缓解

| # | 风险 | 等级 | 缓解措施 |
|---|---|---|---|
| **R1** | glboom/DSDA-Doom 真实 GL 调用面未知，可能出现未预期的扩展依赖 | 高 | M3 先做 Mesa llvmpipe 直通 + GL trace，拿到精确清单后再实现。**不可跳过** |
| **R2** | 8 倍压缩率不能稳定达成（大片火焰/传送门等高频内容） | 高 | M1 就做压缩率实测工具；准备动态降级（渲染距离 → 关双线性 → 跳过行列 → 降帧率） |
| **R3** | GLES 3.0 前端本质是写一个 GLSL 编译器，工作量独立且与 Doom 无关 | 中 | 排在 M6 最后；定义明确的"可识别子集"，超出部分回退 VM 且明确标注性能损失 |
| **R4** | DSDA-Doom fork 的维护成本；Windows 构建链老化 | 中 | patch 做成 `git format-patch` 便于同步上游；CMake preset + vcpkg 固化依赖 |
| **R5** | USB FS 带宽抖动（Windows 调度 + TinyUSB 任务优先级） | 中 | 4KB URB、双缓冲、自适应帧率（22–26fps 浮动）、超时重传、背压丢帧 |
| **R6** | DSDA-Doom 可能使用 `glBegin/glEnd` 立即模式路径，逐顶点发命令会炸带宽 | 中 | 驱动层做显示列表缓存：检测到静态几何时缓存成顶点缓冲，仅在中段变化时重传 |

---

## 10. 目录结构规划

```
espgpu/
├── firmware/                    ESP-IDF 工程
│   ├── CMakeLists.txt
│   ├── main/
│   │   ├── app_main.c
│   │   ├── usb/                 TinyUSB vendor class + WCID 描述符
│   │   ├── proto/               命令流解析 + CRC + 环形缓冲
│   │   ├── gpu/
│   │   │   ├── state.c          管线状态机
│   │   │   ├── tl.c             顶点变换（IRAM）
│   │   │   ├── raster.c         tile 光栅器（IRAM）
│   │   │   ├── tex.c            纹理 LRU
│   │   │   └── fb.c             帧缓冲 + FBO
│   │   ├── codec/               差分 + RLE 编码器
│   │   └── mgmt/                设备信息 / 统计 / OTA
│   └── partitions.csv
├── host/                        Windows 端
│   ├── espgpu/                  espgpu.dll（设备接口层）
│   │   ├── device.cpp           WinUSB 枚举 + overlapped I/O
│   │   ├── cmd.cpp              命令编码
│   │   ├── frame.cpp            帧解码 + CLUT 转换
│   │   └── stats.cpp
│   ├── gl12/                    opengl32.dll（GL 1.2 前端）
│   │   ├── state.cpp            矩阵栈 / 状态缓存
│   │   ├── texenv.cpp           texenv → 内置模式映射
│   │   ├── trace.cpp            GL trace + Mesa 直通
│   │   └── exports.def
│   ├── gles3/                   libGLESv2.dll（GLES 3.0 前端，M6）
│   │   ├── glsl/                词法 / 语法 / 类型检查 / 模式识别
│   │   └── vulkan_like.cpp
│   ├── panel/                   espgpu-panel.exe（ImGui）
│   │   ├── app.cpp
│   │   ├── pages/               设备 / 性能 / 显示 / 调试 / OTA / 时钟 / 日志
│   │   └── viewer.cpp           帧查看器（显示器）
│   └── tests/                   压缩率实测、带宽 benchmark、回环测试
├── third_party/
│   └── dsda-doom/               fork（submodule 或 patch 集）
└── docs/
    ├── protocol.md
    └── gl-coverage.md
```

---

## 11. 待定项

| 项 | 说明 | 建议默认值 |
|---|---|---|
| 音频回传 | Doom 有音效和 MIDI 音乐。走 USB 需独立等时端点，会抢带宽 | **不做**，PC 端本地播放 |
| 多块 S3 并行 | 多设备分摊 tile | **不做**，单设备 |
| 面板 UI 语言 | 中文 / 英文 / 双语 | 英文（技术术语更准确） |
| 项目代号 | 对外命名 | `espgpu` |
| Doom 输入延迟容忍度 | USB 往返 + 压缩约 30–50ms | 先实测再优化 |
| 是否需要 3D demo 素材 | M5 验收需要 | 自研简单场景（旋转立方体 + 地形 + 光照） |

---

## 12. 下一步

进入实施需要切换到执行模式（当前处于 Plan 模式，不能改代码）。

**M1 第一个动作**：

1. 建 `firmware/` ESP-IDF 工程骨架，跑通 `hello_world` + PSRAM 自检。
2. 写 TinyUSB vendor class 描述符 + MS OS 2.0 Descriptor，验证 Windows 免驱识别出 `ESP32-S3 GPU`。
3. 实现 EP1/EP2 的 bulk 回环，用 `espgpu-panel` 的带宽页测出真实吞吐。
4. 生成 320×200 测试图案（渐变 + 棋盘 + 移动方块），分别用 RGB565 和 INDEX8 裸传，记录基线带宽。
5. 加入差分 + RLE 编码，对比压缩率，确定是否需要在 M2 引入 LZ。
