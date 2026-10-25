# ESP32-S3-GPU

用一块 ESP32-S3-WROOM-1-N16R8 做成一块真正的 GPU：Windows 程序通过标准 OpenGL 接口提交绘制命令，ESP32-S3 完成顶点变换与光栅化，渲染结果经 USB 回传到 PC 显示。

完整技术方案见 [PLAN.md](PLAN.md)，线协议见 [docs/protocol.md](docs/protocol.md)。

## 当前状态

**M2 — 2D 光栅器 + 命令流（进行中）**

| 项 | 状态 |
|---|---|
| 设备：命令流执行（CLEAR / PIPELINE2D / BIND_TEXTURE / UPLOAD_TEXTURE / DRAW / SCISSOR / PRESENT） | ✅ |
| 设备：2D 光栅器（边缘函数内测试、最近邻/双线性、alpha 混合、alpha 测试、scissor） | ✅ |
| 设备：内部 SRAM 工作表面（128 KB，实测确认未退回 PSRAM） | ✅ |
| 设备：RGB565 / RGBA4444 纹理，16 位像素 + 独立 alpha 平面 | ✅ |
| 主机：`espgpu_cmd_*` 批量命令 API | ✅ |
| 主机：`espgpu-demo.exe` 2D 演示（滚动纹理 / 旋转 alpha 精灵 / scissor 进度条） | ✅ |
| 设备：纹理 LRU 换页 | ⬜ M3 |
| 性能优化（IRAM 热循环、tile 化） | ⬜ 待做 |

实测：320×200 RGB565，静态棋盘背景 + 旋转 alpha 精灵 + 进度条，
**23.8 fps**（目标 24fps），每帧编码后约 7 KB。完整优化记录见
[docs/perf-notes.md](docs/perf-notes.md)。

24fps 锁定下的耗时构成（设备侧实测）：

```
光栅化 16.5 ms   编码 6.8 ms   USB 8.9 ms
```

设备侧合计 32 ms，帧预算 41.7 ms，约 10 ms 余量。带宽已不再是瓶颈。
病态负载（`--stress`：全屏每帧滚动）的链路上限是 4.6 fps / 465 KB/s。

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
  main/            app、USB、GPU 状态、帧缓冲、编码
  components/      仅做头文件导出的组件（把 shared/ 接进固件）
shared/            协议定义，固件与主机端共用同一份头文件
host/              Windows 端
  include/espgpu/  公开 C API
  src/             espgpu.dll：WinUSB、链路、解码
  panel/           espgpu-panel.exe：控制面板 + 帧查看器
  tests/           espgpu-bench.exe
docs/              协议文档
tools/             构建脚本
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

- `espgpu.dll`
- `espgpu-panel.exe`
- `espgpu-bench.exe`

ImGui 由 CMake 的 FetchContent 拉取（v1.91.9）。离线环境可以用
`-DIMGUI_DIR=<本地路径>` 指定已有副本。

## 运行

```powershell
# 控制面板（含帧查看器）
.\host\out\build\default\espgpu-panel.exe

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
