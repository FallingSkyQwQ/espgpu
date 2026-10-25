# ESP32-S3-GPU 线协议

> 协议版本：`1`
> 权威定义：`shared/espgpu_proto.h`（固件与主机端共用同一份头文件）

本文档解释设计意图；字段的权威定义永远以头文件为准。

---

## 1. 传输层

设备是一个 USB Full-Speed vendor class 设备：

| 项 | 值 |
|---|---|
| VID / PID | `0x303A` / `0x8201` |
| 设备类 | `0x00`（类信息定义在接口上） |
| 接口 0 | vendor specific (`0xFF`) |
| EP1 OUT | bulk, 64B MPS — 主机 → 设备 |
| EP2 IN | bulk, 64B MPS — 设备 → 主机 |
| 驱动绑定 | WCID / MS OS 2.0 → `winusb.sys`，免驱免签 |

USB 层已经有 CRC5/CRC16 保护，所以协议层的 CRC 是可选的（`ESPGPU_F_CRC`），
默认关闭以省 CPU。

---

## 2. Chunk 层

两条 bulk 端点传输的都是**同一种分帧格式**的字节流。设备重启、线缆抖动
或半截包之后，接收方通过滑动窗口搜索魔数自愈，不需要额外握手。

```
偏移  大小  字段
0     4     magic       H2D = 0x47505345 ("ESPG"), D2H = 0x46505345 ("ESPF")
4     2     version     协议版本，不匹配即视为失步
6     2     seq         单调递增，用于检测丢片
8     2     length      载荷字节数
10    1     type        ESPGPU_CHUNK_*
11    1     flags       ESPGPU_F_*（MORE / CRC / ACK / URGENT）
12    N     payload
12+N  2     crc16       CRC16-CCITT，仅当 flags 带 ESPGPU_F_CRC 时存在
```

`length` 上限 `ESPGPU_CHUNK_MAX_PAYLOAD` = 32 KB。更大的数据必须分片。

### Chunk 类型

| 方向 | 值 | 名称 | 载荷 |
|---|---|---|---|
| H2D | `0x01` | `CMD` | 一串 `espgpu_cmd_t` |
| H2D | `0x02` | `DATA` | 纹理 / 顶点缓冲 / 固件镜像 |
| H2D | `0x03` | `CTRL` | `espgpu_ctrl_req_t` + 参数 |
| D2H | `0x81` | `FRAME` | `espgpu_frame_hdr_t` + 分片数据 |
| D2H | `0x82` | `EVENT` | `espgpu_event_t` |
| D2H | `0x83` | `CTRL_RESP` | `espgpu_ctrl_resp_t` + 数据 |
| D2H | `0x84` | `LOG` | UTF-8 文本 |
| D2H | `0x85` | `CLUT` | 256 × RGB565（512 字节） |

---

## 3. 控制通道

`CTRL` 是同步请求/响应模型，用 `seq` 配对。

```
请求：espgpu_ctrl_req_t { u16 op; u16 seq; u32 arg0; u32 arg1; }  + 参数
响应：espgpu_ctrl_resp_t { u16 op; u16 seq; u16 status; u16 len; } + 数据
```

| op | 名称 | arg0 | 参数 | 响应 |
|---|---|---|---|---|
| `0x0001` | `PING` | — | — | 回显请求结构 |
| `0x0002` | `GET_DEVICE_INFO` | — | — | `espgpu_device_info_t` |
| `0x0003` | `GET_STATS` | — | — | `espgpu_stats_t` |
| `0x0004` | `SET_MODE` | 图案 ID | `espgpu_mode_t` | 空 |
| `0x0005` | `SET_CLUT` | — | 256 × u16 | 空 |
| `0x0006` | `RESET_STATS` | — | — | 空 |
| `0x0007` | `SET_CLOCK` | MHz | — | 空（M4） |
| `0x0010` | `OTA_BEGIN` | 总长度 | 版本串 | 空（M4） |
| `0x0011` | `OTA_DATA` | 偏移 | 数据块 | 空（M4） |
| `0x0012` | `OTA_END` | CRC | — | 空（M4） |

状态码见 `ESPGPU_OK` / `ESPGPU_ERR_*`。

---

## 4. 帧通道

一帧可能被切成多个 `FRAME` Chunk。每个分片都重复完整帧头，
这样单个包就可以独立解析，丢片也能立刻发现。

```
espgpu_frame_hdr_t {
    u32 frame_id;
    u16 width, height;
    u16 origin_x, origin_y, origin_w, origin_h;   /* 预留局部更新 */
    u32 data_len;    /* 本分片内的像素数据字节数 */
    u32 raw_len;     /* 整帧裸数据字节数 */
    u8  format;      /* ESPGPU_FMT_* */
    u8  codec;       /* ESPGPU_CODEC_* */
    u8  frag_index;
    u8  frag_count;
    u8  flags;       /* ESPGPU_FF_FIRST / LAST / DROPPED / VSYNC */
    u8  palette_dirty;
}
```

分片规则：

- `frag_index == 0` 必须带 `ESPGPU_FF_FIRST`
- 最后一个分片必须带 `ESPGPU_FF_LAST`
- 接收方在 `LAST` 到达时才解码；`frag_index` 不连续则整帧作废

### 编码

| codec | 说明 |
|---|---|
| `RAW = 0` | 裸数据 |
| `RLE = 1` | 像素级游程 |
| `DELTA_RLE = 2` | 与上一帧逐像素异或后 RLE |
| `TILE_DELTA = 3` | tile 脏标记 + 每 tile RLE（M2） |

RLE 的字节格式（`bpp` = 每像素字节数）：

```
控制字节 c
  c & 0x80 : 重复游程，像素数 = (c & 0x7F) + 2     ∈ [2, 129]
             后接 bpp 字节的像素值
  其它     : 字面游程，像素数 = c + 1              ∈ [1, 128]
             后接 count × bpp 字节原始像素
```

最坏情况膨胀为 `bpp + 1` 字节/像素，所以编码缓冲按
`size + size/bpp` 分配。

差分模式的参考帧是**上一帧成功发送出去的帧**。设备只有在
`send_frame` 返回成功后才交换双缓冲，所以丢帧不会让参考帧失配。

---

## 5. 像素格式

| 值 | 格式 | 字节/像素 |
|---|---|---|
| 1 | `INDEX8` | 1 |
| 2 | `RGB565` | 2 |
| 3 | `RGBA4444` | 2 |
| 4 | `RGBA8888` | 4 |
| 5 | `R8` | 1 |
| 6 | `DEPTH16` | 2 |

`INDEX8` 配合 `CLUT` Chunk 使用。Doom 这类调色板渲染的内容走这条路径
可以把带宽再砍一半，同时大幅提升 RLE 压缩率。

---

## 6. 带宽预算

USB Full-Speed bulk 实测 **0.85–1.0 MB/s**。目标 320×200 @ 24fps 时，
每帧总预算约 35 KB（下行命令 + 上行帧合计）。

`espgpu-bench` 就是用来验证这条预算线的：它把每种图案 × 编码 × 格式
跑一遍，输出实测帧率、上行带宽和压缩比。

---

## 7. 兼容性约定

协议一旦发布，字段只允许**追加**，不允许修改语义或删除。新增能力通过：

- `espgpu_device_info_t.capabilities` / `.codecs` 位掩码上报
- 未知 opcode / chunk type 必须被安全忽略

这样固件与主机端可以独立升级。

---

## 8. M2–M6 追加（向后兼容）

所有追加都遵守第 7 节：只追加字段 / opcode / 位，旧主机发的命令照旧合法。
设备通过 `capabilities` 上报支持情况（`ESPGPU_CAP_TNL / TEX_LRU / PALETTED_TEX /
BUFFERS / CLOCK / DEBUG_VIEW / LZ / LOG`）。

### 8.1 3D 绘制（T&L 在设备上）

一次 3D 绘制 = `PIPELINE` + `VERTEX_LAYOUT` + `SET_MATRIX` + `DRAW(flags=ESPGPU_DRAWF_3D)`。
`DRAW` 不带 `ESPGPU_DRAWF_3D` 时仍是 M2 的 2D 顶点（`espgpu_vertex2d_t`），走 `PIPELINE2D`。

| op | 命令体 | 说明 |
|---|---|---|
| `0x01 PIPELINE` | `espgpu_pipeline_t` [+ `espgpu_pipeline_fog_t`] [+ `espgpu_pipeline_ext_t`] | `mask` 选择更新哪些组；追加段按长度判断 |
| `0x02 SET_MATRIX` | `espgpu_set_matrix_t` + 16 × (int32 16.16 或 float) | `ESPGPU_MATF_FLOAT` / `TRANSPOSE` / `IDENTITY`；槽位 MVP / MODELVIEW / PROJECTION / TEXTURE0 / NORMAL / TEXTURE1 |
| `0x03 VERTEX_LAYOUT` | `espgpu_vertex_layout_t` | 原 `pad` 字段改为 `frac_bits`：整数属性的定点小数位 |
| `0x06 DRAW` | `espgpu_draw_t` + 顶点 + 索引 | 内联或 `ESPGPU_VSRC_BUFFER`（设备端缓冲） |
| `0x08 VIEWPORT` | `espgpu_rect_t` | 屏幕目标左上角原点；FBO 纹理目标左下角原点 |
| `0x0B FBO` | `espgpu_fbo_t` | 渲染到纹理 / 回屏幕 / 拷贝到纹理 |
| `0x0E FENCE` | `espgpu_fence_t` | 设备回 `EVENT{FENCE, arg=value}` |
| `0x11 TEX_PALETTE` | `espgpu_tex_palette_t` + RGB565[] + alpha[] | INDEX8 纹理的调色板 |
| `0x12 DELETE_TEXTURE` | `espgpu_delete_texture_t` | `0xFFFFFFFF` = 全部 |
| `0x13 COLOR` | `espgpu_color_t` | 布局里没有颜色属性时的常量色 |
| `0x14 LIGHTING` | `espgpu_lighting_t` + `espgpu_light_t`[] | 眼空间方向光，最多 4 盏 |
| `0x15 LOAD_PROGRAM` | `espgpu_load_program_t` + `espgpu_vm_insn_t`[] | 片元 VM，见 `shared/espgpu_vm.h` |
| `0x16 SET_UNIFORMS` | `espgpu_set_uniforms_t` + vec4[] | VM 常量寄存器 c0..c31 |
| `0x17 DEPTH_RANGE` | `espgpu_depth_range_t` | glDepthRange |

管线的完整语义（坐标系、绕序、着色模式公式、雾 / alpha / 深度 / 混合的顺序）写在
`espgpu_proto.h` 里 `ESPGPU_SHADE_TEX_DECAL` 下面的注释块，前端必须按它实现。

设备内部把三角形分到 64×64 的 tile，`PRESENT`（或分箱内存满、纹理被改写、FBO 切换）
时才逐 tile 光栅化。所以命令的副作用只在 `PRESENT` 之后可见，`FENCE` 只保证命令已被执行到。

### 8.2 纹理

`UPLOAD_TEXTURE.format` 新增 `INDEX8`（配 `TEX_PALETTE`）和 `R8`（亮度）。
命令头 `flags` 的 `ESPGPU_TEXF_PRIORITY` 让 LRU 最后才换出它。

纹理缓存总量是 `device_info.texture_cache_size`。超出时设备换出最久未用的纹理并发
`EVENT{TEXTURE_EVICTED, arg=id}`；主机再次使用前必须重传。主机前端通常维护同样的
LRU 镜像、提前 `DELETE_TEXTURE`，设备侧换出只是兜底。被换出的纹理采样结果为白色。

### 8.3 设备端缓冲（DATA chunk）

```
DATA 载荷 = espgpu_data_hdr_t { u32 buffer_id; u32 offset; u32 total_len; u8 kind; u8 flags; u16 pad }
          + 数据
```

`offset == 0` 的分片负责（重新）分配，之后按 `offset` 续写；`flags & ESPGPU_DATAF_DELETE`
释放。内存不足时设备回 `EVENT{BUFFER_EVICTED, arg=id}`。`DRAW` 用
`source = ESPGPU_VSRC_BUFFER` 引用：顶点从 `base_vertex * stride` 开始，索引在同一个缓冲的
`index_offset` 处。用于显示列表 / 静态几何（PLAN R6），每帧只发一条十几字节的 DRAW。

### 8.4 下行压缩

`CMD` / `DATA` chunk 的 `flags` 可以带 `ESPGPU_F_LZ`（`0x10`）：载荷 = `u32 解压后长度` +
LZ4 block（`shared/espgpu_lz.h`，固件与主机共用）。主机只在设备上报 `ESPGPU_CAP_LZ`
且压缩后更小时才这样发。解压后长度上限 64 KB。

### 8.5 控制请求追加

| op | 名称 | arg0 | 参数 | 说明 |
|---|---|---|---|---|
| `0x0007` | `SET_CLOCK` | MHz | — | 80 / 160 / 240，esp_pm 锁频 |
| `0x0010` | `OTA_BEGIN` | 总长度 | 版本串 | 顺序写模式，边写边擦 |
| `0x0011` | `OTA_DATA` | 偏移 | 数据块 | 偏移必须连续 |
| `0x0012` | `OTA_END` | CRC-32（0 = 不校验） | — | 校验后切换启动分区 |
| `0x0013` | `OTA_ABORT` | — | — | |
| `0x0020` | `LOG_SUBSCRIBE` | 级别（0 = 取消） | — | 设备日志以 `LOG` chunk 推送 |
| `0x0030` | `SOFT_RESET` | — | — | 先回响应再重启 |
| `0x0031` | `REQUEST_KEYFRAME` | — | — | 下一帧发非差分帧 |
| `0x0040` | `SET_DEBUG_VIEW` | `ESPGPU_DEBUG_VIEW_*` | — | 深度 / overdraw / tile / 线框，只影响回传画面 |
| `0x0041` | `SET_CODEC` | `ESPGPU_CODEC_*` | — | 命令模式下 PRESENT 的编码 |

OTA 的 CRC 是标准 CRC-32（与 zlib、`esp_rom_crc32_le` 一致）。新固件启动后调用
`esp_ota_mark_app_valid_cancel_rollback()`；如果新固件起不来，bootloader 自动回滚。

### 8.6 事件追加

| kind | 名称 | arg |
|---|---|---|
| 6 | `TEXTURE_EVICTED` | 纹理 id |
| 7 | `BUFFER_EVICTED` | 缓冲 id |

### 8.7 统计追加

`espgpu_stats_t` 在 `seq_gaps` 之后追加了纹理缓存、三角形 / 剔除数、分箱内存、T&L 耗时、
CPU 频率、调试视图、OTA 状态、缓冲占用、LZ 字节数。主机收到较短的响应（老固件）时
缺失字段视为 0。

### 8.8 帧号

命令模式和测试图案的帧号都从 1 开始，主机侧 `espgpu_frame_id() == 0` 表示还没收到过帧。

### 8.9 有损块编码与自适应编码（issue #2）

USB FS 实测 ~0.75 MB/s，24 fps 时每帧预算 ~31 KB；320x200 RGB565 镜头一动整屏都变，
带纹理的画面用无损 RLE 只能压到 1.0–1.5 倍（仿 Doom 画面实测 1.10 倍，117 KB/帧）。

| codec | 值 | 说明 |
|---|---|---|
| `BLOCK` | 4 | 有损。载荷 = 每 4x4 块一位的位图 + 每个置位块 8 字节 BC1。关键帧 = 位图全置位；差分帧只带变化的块（按设备上的**源图**比较，误差不累积）。320x200 整帧最多 32.5 KB，静止画面只有 500 B 位图。格式见 `shared/espgpu_block.h`（固件、主机、模拟器共用）。 |
| `AUTO` | 5 | 只作模式值（`SET_MODE.codec` / `SET_CODEC`），不出现在帧头。差分帧放得进预算时发无损 `DELTA_RLE`；放不下就发 `BLOCK` 关键帧并转入有损状态；有损状态下画面连续静止几帧后补一帧无损 `RLE` 关键帧，回到无损。预算 = 720 KB/s × max(1 / `mode.max_fps`, 设备生产一帧的实测忙碌时间)：设备只有几 fps 时（Doom 320x200）上行有富余，差分帧大一些也照发无损（issue #5）。忙碌时间不含等发送槽位、等主机命令的时间，否则会正反馈。 |

设备在 `device_info.codecs` 里用 `ESPGPU_CAP_CODEC_BLOCK / AUTO` 上报支持。主机的
`espgpu_preferred_codec()` 在设备支持时返回 `AUTO`（环境变量 `ESPGPU_CODEC=auto|block|delta|rle|raw`
可覆盖），GL 1.2 / GLES 前端和 demo 都用它。主机解码器保证 `DELTA_RLE` 只会接在无损帧后面；
收到不合规的流会丢帧并请求关键帧。实测画质（BC1 + 一次最小二乘端点修正）约 32 dB PSNR。

### 8.10 控制请求不再排在整帧之后（issue #3）

固件拆成接收任务（读 USB chunk，core0，优先级 8）和命令任务（执行命令，优先级 7）。
控制请求在接收任务里当场处理并回响应，渲染一帧期间也能及时回 `GET_STATS` 等；
`CMD` / `DATA` 以及会改渲染状态的 `SET_MODE` / `SET_DEBUG_VIEW` 按到达顺序进队列交给命令任务。
接收侧有 4 个 32 KB 缓冲；命令任务全部占满时接收任务停止读 USB，对主机形成背压。

### 8.11 命令模式与测试图案模式的切换（issue #4）

- 收到第一个 `CMD` chunk：进入命令模式（测试图案停发，下一帧强制关键帧）。
- 收到 `GET_DEVICE_INFO`（每次 `espgpu_open()` 的握手）或 `SET_MODE`：退出命令模式，
  测试图案约 250 ms 后恢复（期间如果又来了命令就直接留在命令模式，不会闪一帧测试图案）。
  主机侧 WinUSB 是独占的：换了程序或改了模式，就说明之前的命令流已经结束。

### 8.12 帧节奏、编码并行与像素管线统计（issue #5）

- **主机节奏**：GL 1.2 前端和 `espgpu-demo3d` 每帧先把命令和 `PRESENT` 发出去，再发一个
  `FENCE`，然后等**上一帧**的 `FENCE`（设备光栅 + 编码完、交给发送任务时回）。设备光栅 / 回传
  上一帧时这一帧的命令已经在路上，下行、光栅、上行三段不再串行；主机最多领先设备一帧，
  设备的发送槽位满了命令任务自己会等，排队有上限。
- **FENCE 事件**：设备上的事件改由发送任务在帧之间 / 分片之间发出（命令任务不再等 USB 的
  TX 互斥量）。新固件的 FENCE 事件 `code` 带 `ESPGPU_EVTF_FENCE_FRAME`，`value` = 执行这个
  FENCE 时最后产出的帧号低 16 位；`espgpu_wait_fence_frame()` 用它等到那一帧真正收到
  （GL 1.2 的 `glReadPixels`）。退出命令模式时清掉没发出去的事件。
- **编码并行**：`PRESENT` 时 core1 的光栅 worker 空闲，`BLOCK` / `RLE` / `DELTA_RLE` 都把一帧
  切成上下两半在两个核上同时编码，再首尾拼接（游程流按像素顺序，拼接后仍合法；`BLOCK`
  的分界块编号是 8 的倍数，两半的位图字节不重叠）。
- **统计**：`espgpu_stats_t` 追加 `t_px_seg_us`（像素管线每段的属性建立：透视除法、定点化）、
  `t_px_pix_us`（深度测试之后的逐像素内核）、`px_rows` / `px_segs` / `px_tested`。
  都是两个 worker 相加，属于 `t_tile_us` 的一部分；固件串口每 30 帧多打一行 `px:`。

