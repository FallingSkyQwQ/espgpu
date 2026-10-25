# ESP32-S3-GPU

用一块 ESP32-S3-WROOM-1-N16R8 做成一块真正的 GPU：Windows 程序通过标准 OpenGL 接口提交绘制命令，ESP32-S3 完成顶点变换与光栅化，渲染结果经 USB 回传到 PC 显示。

完整技术方案见 [PLAN.md](PLAN.md)，线协议见 [docs/protocol.md](docs/protocol.md)。

## 当前状态

> M2–M6 的代码已全部写完，交叉编译通过；设备核心在主机上有渲染冒烟测试。
> **尚未在真实硬件上验证**：性能数字、DSDA-Doom 帧率、OTA / 调频都需要接板子实测。

| 里程碑 | 内容 | 代码 | 硬件验证 |
|---|---|---|---|
| M1 | 链路打通 | ✅ | ✅ |
| M2 | tile 光栅器（透视校正 / 双线性 / alpha / Z）、纹理 LRU、CLEAR/DRAW/SWAP、500 drawcall demo | ✅ | ⬜ |
| M3 | `opengl32.dll`：GL 1.2 固定管线 + GL trace + Mesa 直通 | ✅ | ⬜ |
| M4 | DSDA-Doom 接入（`opengl32.dll` 直接替换 + espgpu 呈现后端） | ✅（见 [docs/doom.md](docs/doom.md)） | ⬜ |
| M5 | T&L、视锥裁剪、背面剔除、高洛德光照、FBO；面板全功能（Material 3 重写） | ✅ | ⬜ |
| M6 | GLES 3.0：`libGLESv2.dll` + `libEGL.dll` + GLSL ES 编译器 + 片元 VM | ✅（见 [docs/glsl.md](docs/glsl.md)） | ⬜ |

### 设备核心（`firmware/core/`）

固件和主机端软件模拟器编译**同一份**可移植 C 代码：

```
命令流 → T&L（MVP / 光照 / 纹理矩阵 / 雾）→ 齐次裁剪（近远 + 保护带）→ 剔除
      → 三角形建立（边函数 + 属性平面）→ 64×64 tile 分箱
PRESENT → 逐 tile（双核领取）：按行解析求跨度，每 8 像素一次透视除法
      → 着色（9 种内置模式或片元 VM）→ 雾 → alpha 测试 → 深度 → GL 混合
      → 差分 + RLE 编码 → 发送任务（与下一帧的命令执行重叠）
```

- 屏幕目标优先放内部 SRAM，光栅器直写；放不下（大分辨率）就走 tile 拷入拷出
- 纹理：RGB565 / RGBA4444 / INDEX8（每纹理调色板）/ R8，PSRAM 里 4 MB LRU
- 设备端缓冲（显示列表 / 静态几何）、下行 LZ 压缩、调试视图（深度 / overdraw / tile / 线框）
- 管理：OTA（带回滚）、80/160/240 MHz 调频、设备日志转发

### 帧编码（USB 带宽）

USB Full-Speed 实测只有 ~0.75 MB/s，24 fps 时每帧 ~31 KB。镜头一动整屏都变，带纹理的画面
无损压不进去，所以命令模式默认用 **AUTO**：静止或局部变化时发无损 DELTA_RLE，整屏在动时
改发有损的 BLOCK（BC1 4x4 块，固定 4:1，没变的块跳过，320x200 每帧最多 32.5 KB），
画面停下来后再补一帧无损关键帧。设备本身只有几 fps 时（上行有富余）也照发无损。
编码在两个核上并行。想强制某种编码可设环境变量：

```powershell
$env:ESPGPU_CODEC = "delta"   # auto | block | delta | rle | raw
```

### 没有硬件时

设环境变量 `ESPGPU_EMULATOR=1`，`espgpu.dll` 会在进程内跑设备核心，所有前端、demo、
面板都能端到端运行，渲染结果与设备逐像素一致（同一份光栅代码），只是没有 USB 带宽限制。

**M1 — 链路打通（完成，已在真实硬件上验证）**

| 项 | 状态 |
|---|---|
| 固件：TinyUSB vendor class + WCID 免驱 | ✅ 已验证 |
| 固件：Chunk 分帧、控制通道、测试图案、RLE / 差分编码 | ✅ 已验证 |
| 固件：PSRAM 双缓冲、温度/内存统计 | ✅ 已验证 |
| 主机：`espgpu.dll` 设备层（WinUSB + 后台接收线程 + 帧解码） | ✅ 已验证 |
| 主机：`espgpu-panel.exe` 控制面板 / 帧查看器（Win32 + DX11 + ImGui） | ✅ 已验证 |
| 主机：`espgpu-bench.exe` 带宽与压缩率实测 | ✅ 已验证 |

实测摘要（完整数据见 [docs/bench-results.md](docs/bench-results.md)）：

- USB 上行峰值 **768 KB/s**
- 320×200 INDEX8 + DELTA_RLE + Doom 近似内容：**43.7 fps / 214 KB/s / 12.8x**
- 320×200 RGB565 + DELTA_RLE + 同内容：**29.9 fps / 241 KB/s / 15.3x**

结论：24fps 目标有约 45% 带宽余量，瓶颈将从帧回传转移到 M2 的下行命令流。

后面的里程碑（2D 光栅器、GL 1.2 前端、Doom、3D、GLES 3.0）见 PLAN.md。

## 目录

```
firmware/          ESP-IDF 固件（ESP32-S3）
  core/            设备核心（可移植 C）：T&L、tile 光栅、纹理 LRU、片元 VM
  main/            app、USB、命令 / 帧发送任务、OTA、调频、日志
  components/      仅做头文件导出的组件（把 shared/ 接进固件）
shared/            协议、片元 VM 指令集、LZ 编解码，固件与主机端共用
host/              Windows 端
  include/espgpu/  公开 C API
  src/             espgpu.dll：WinUSB、软件模拟器、链路、解码、命令打包
  gl12/            opengl32.dll：GL 1.2 前端（含 trace / Mesa 直通）
  glsl/            GLSL ES 3.00 编译器（静态库）
  gles3/           libGLESv2.dll + libEGL.dll + GLES demo
  panel/           espgpu-panel.exe：Material 3 控制面板 + 帧查看器
  demo/ demo3d/    2D / 3D 演示
  tests/           espgpu-bench.exe、设备核心渲染测试
docs/              协议、GL 覆盖面、GLSL、Doom、性能记录
tools/             构建脚本、GL 入口生成器、Doom 配置
```

## 构建

### 固件

需要 ESP-IDF v6.x。

```powershell
. .\tools\idf-env.ps1
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

> 必须接板子的**原生 USB 口**（ESP32-S3 的 GPIO19/20），不是 UART 口。
> 原生口负责 vendor 数据链路，UART 口只用于看日志。

### Windows 端

需要 Visual Studio Build Tools（C++ 工作负载）+ Windows SDK。

```powershell
.\tools\build-host.ps1
```

产物在 `host\out\build\default\`：

- `espgpu.dll`、`espgpu-panel.exe`、`espgpu-bench.exe`
- `espgpu-demo.exe`（2D）、`espgpu-demo3d.exe`（3D）、`espgpu-core-test.exe`
- `gl12\opengl32.dll`（GL 1.2 前端）
- `gles3\libGLESv2.dll`、`gles3\libEGL.dll`、`gles-triangle.exe`、`gles-shader.exe`

ImGui 由 CMake 的 FetchContent 拉取（v1.91.9）。离线环境可以用
`-DIMGUI_DIR=<本地路径>` 指定已有副本。

## 运行

```powershell
# 控制面板（含帧查看器）
.\host\out\build\default\espgpu-panel.exe

# M2 验收：500 个独立 drawcall
.\host\out\build\default\espgpu-demo.exe --drawcalls 500

# M5 验收：3D 场景（自带窗口；面板此时连不上设备，WinUSB 是独占的）
.\host\out\build\default\espgpu-demo3d.exe

# 没有硬件：用软件模拟器跑任何程序
$env:ESPGPU_EMULATOR = "1"

# 设备核心渲染测试（输出 core_*.bmp）
.\host\out\build\default\espgpu-core-test.exe

# 带宽 / 压缩率实测，每种组合默认 3 秒
.\host\out\build\default\espgpu-bench.exe
.\host\out\build\default\espgpu-bench.exe -s 5 -i
```

设备免驱：Windows 10/11 会自动通过 MS OS 2.0 描述符绑定 `winusb.sys`，
不需要安装任何驱动，也不需要签名。设备管理器里会显示为 `ESP32-S3 GPU`。

## M1 硬件验证清单

按顺序检查，任何一步失败都能定位到具体层：

1. **枚举**：设备管理器出现 `ESP32-S3 GPU`（通用串行总线设备下），
   硬件 ID 为 `USB\VID_303A&PID_8201`，驱动是 WinUSB。
   - 若显示为未知设备 → MS OS 2.0 描述符没生效，看串口日志里有没有
     `MS OS 2.0 descriptor requested`
2. **握手**：`espgpu-bench -i` 能打印出设备信息（固件版本、PSRAM、能力位）。
3. **吞吐**：`espgpu-bench` 跑完，看 `bars` / `ui` 图案在 320x200 INDEX8 +
   DELTA_RLE 下能否达到 ≥24fps 且 ≤850 KB/s。这组数据决定 M2 的性能预算。
4. **画质**：`espgpu-panel.exe` 的 Viewer 页面能看到测试图案，切换图案/格式/
   编码应立即生效，不应出现花屏（花屏说明差分参考帧失配）。
5. **温度**：Overview 页面能读到芯片温度，Performance 页面的曲线随负载变化。

实测数据出来后，把 `espgpu-bench` 的输出贴到 issue 或直接更新本文件，
M2 的光栅器性能预算以它为准。

## 设计要点

1. **两端 GL 共用一个设备核心**。GL 1.2 和 GLES 3.0 只是两个前端，
   设备协议只设计一次。
2. **全链路 8-bit 调色板**。纹理和帧都可以走 `INDEX8`，带宽和 PSRAM 占用各砍一半。
3. **GPU 端做顶点变换**。世界坐标用 int16 定点 + 每次 draw 一个矩阵，
   带宽与"主机端预变换"相同，但主机的 CPU 省下来了。
4. **tile 渲染到内部 SRAM，DMA 回写 PSRAM**。绝不让光栅器直写 PSRAM，
   否则会和 USB DMA 抢总线。

## 许可

见 [LICENSE](LICENSE)。
