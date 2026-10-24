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
