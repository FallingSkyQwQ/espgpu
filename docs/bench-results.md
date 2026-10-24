# M1 实测结果

硬件：ESP32-S3-WROOM-1-N16R8（rev 0.2），8MB Octal PSRAM @80MHz
主机：Windows 11 (10.0.26300)，WinUSB 免驱
固件：0.1.0，协议 1
测量工具：`espgpu-bench -s 3`（每种组合 3 秒）

## 结论摘要

| 指标 | 实测值 | 计划假设 | 判定 |
|---|---|---|---|
| USB 上行峰值 | **768 KB/s** | 850 KB/s | 略低于假设，按 750 KB/s 重新预算 |
| Doom 近似内容压缩比 | **12.8 – 16.4x** | ≥ 8x | 显著优于假设 |
| 320×200 24fps 余量 | 43.7 fps @ 214 KB/s | 24 fps | 约 45% 带宽余量 |
| 噪声（最坏情况）压缩比 | 1.0x | 1.0x | 符合预期 |

**关键判断：320×200 @ 24fps 的目标可以轻松达成，瓶颈不再是帧回传，而是
M2 的命令流带宽。** 按 24fps 锁定后帧回传约 118 KB/s，还剩约 630 KB/s
给下行命令，需要把每帧命令流压到 26 KB 以内（约 500 个四边形）。

## 完整矩阵

### 320×200

| 格式 | 编码 | 图案 | fps | KB/s | 压缩比 |
|---|---|---|---|---|---|
| RGB565 | RAW | bars | 5.3 | 664 | 1.0x |
| RGB565 | RAW | ui | 5.3 | 685 | 1.0x |
| RGB565 | RAW | doomish | 5.3 | 664 | 1.0x |
| RGB565 | RAW | noise | 5.3 | 686 | 1.0x |
| RGB565 | RLE | bars | 26.5 | 326 | 10.2x |
| RGB565 | RLE | ui | 25.5 | 402 | 7.9x |
| RGB565 | RLE | doomish | 32.1 | 209 | 18.6x |
| RGB565 | RLE | noise | 5.0 | 625 | 1.0x |
| RGB565 | DELTA_RLE | bars | 17.6 | 408 | 5.3x |
| RGB565 | DELTA_RLE | ui | 34.3 | 246 | 18.0x |
| RGB565 | DELTA_RLE | doomish | **29.9** | 241 | 15.3x |
| RGB565 | DELTA_RLE | noise | 5.0 | 626 | 1.0x |
| INDEX8 | RAW | bars | 11.3 | 704 | 1.0x |
| INDEX8 | RAW | ui | 11.6 | 727 | 1.0x |
| INDEX8 | RAW | doomish | 10.7 | 667 | 1.0x |
| INDEX8 | RAW | noise | 11.6 | 728 | 1.0x |
| INDEX8 | RLE | bars | 47.6 | 382 | 7.8x |
| INDEX8 | RLE | ui | 47.4 | 465 | 6.4x |
| INDEX8 | RLE | doomish | **45.4** | 179 | 16.4x |
| INDEX8 | RLE | noise | 11.0 | 694 | 1.0x |
| INDEX8 | DELTA_RLE | bars | 30.9 | 475 | 4.1x |
| INDEX8 | DELTA_RLE | ui | **52.6** | 205 | 16.6x |
| INDEX8 | DELTA_RLE | doomish | **43.7** | 214 | 12.8x |
| INDEX8 | DELTA_RLE | noise | 10.6 | 668 | 1.0x |

### 160×120

| 格式 | 编码 | 图案 | fps | KB/s | 压缩比 |
|---|---|---|---|---|---|
| RGB565 | RAW | bars | 20.0 | 751 | 1.0x |
| RGB565 | RAW | ui | 20.3 | 761 | 1.0x |
| RGB565 | RAW | doomish | 19.9 | 748 | 1.0x |
| RGB565 | RAW | noise | 20.7 | 769 | 1.0x |
| RGB565 | RLE | bars | 62.5 | 243 | 9.7x |
| RGB565 | RLE | ui | 62.3 | 478 | 5.0x |
| RGB565 | RLE | doomish | 62.5 | 230 | 10.3x |
| RGB565 | RLE | noise | 19.2 | 723 | 1.0x |
| RGB565 | DELTA_RLE | bars | 61.9 | 441 | 5.3x |
| RGB565 | DELTA_RLE | ui | 59.8 | 378 | 6.4x |
| RGB565 | DELTA_RLE | doomish | 62.3 | 267 | 8.7x |
| RGB565 | DELTA_RLE | noise | 18.9 | 714 | 1.0x |

## 值得注意的现象

**DELTA_RLE 在平坦图案上反而更差。** `bars` 图案是水平移动的，整屏每帧都在变，
差分后没有多少零像素；而 `RLE` 直接压平坦色块效果更好（10.2x vs 5.3x）。
真实 Doom 场景是静态几何 + 局部运动，DELTA_RLE 优势明显（16.4x vs 18.6x 接近），
但**不能无脑默认 DELTA_RLE**，M2 需要按内容自适应选择编码。

**INDEX8 不是在所有情况下都赢。** 由于当前测试图案先转 RGB565 再折成灰度索引，
INDEX8 的压缩率优势没有完全体现（真实调色板内容会更好）。但即便在这个不利条件下，
INDEX8 + DELTA_RLE 仍然拿到 43.7 fps / 12.8x，说明调色板路径值得作为默认。

## 引导过程中修掉的固件缺陷

1. **MS OS 2.0 描述符套了 configuration subset header** —— 非复合设备必须用扁平的
   「set header + compatible ID」结构，否则 Windows 静默忽略 Compatible ID，
   表现为设备管理器 Code 28（MS OS 请求正常应答但驱动装不上）。
2. **命令任务被 USB 任务饿死** —— esp_tinyusb 的任务是 `while(1) tud_task();`，
   持续出帧时几乎一直就绪，同核低优先级任务拿不到 CPU。命令任务优先级需高于 5。
3. **呈现任务整帧持有 g_gpu.lock** —— 控制任务直接抢锁会被饿死（实测 SET_MODE
   阻塞 15 秒）。改成控制侧挂待办、呈现任务在帧边界应用。
4. **测试图案坐标数学除零/无符号下溢** —— `h - 120` 在 160×120 时为 0，
   触发 `IntegerDivideByZero` panic；更小分辨率下会下溢成巨大值导致越界写。
