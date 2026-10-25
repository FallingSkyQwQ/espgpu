# 在 espgpu 上跑 DSDA-Doom（M4）

> 目标（PLAN §1）：DOOM2 MAP01 在 DSDA-Doom（prboom-plus 一脉）上可玩，≥ 24 fps 稳帧。
> 状态：代码路径已实现，**尚未在真机上验证**。下面的步骤也是验证清单。

## 原理

不 fork 游戏的渲染器。`host/gl12` 产出一个**替身 `opengl32.dll`**：

```
dsda-doom.exe ──SDL──> LoadLibrary("OPENGL32.DLL")   ← 应用目录优先，拿到的是我们的 DLL
                           │
                           ├─ wgl*：像素格式 / 上下文 / SwapBuffers（gdi32 的 ChoosePixelFormat、
                           │        SetPixelFormat、SwapBuffers 也会转进这里）
                           └─ gl*：固定管线状态机 → espgpu.dll → USB → ESP32-S3
```

`SwapBuffers` 就是「espgpu video backend」：发 PRESENT → 等设备回传新帧（有上限的等待，
同时起背压作用）→ 按 `ESPGPU_FPS` 节流 → `StretchDIBits` 到游戏自己的窗口。
输入、声音、窗口都还是游戏原样的，所以不需要改 DSDA-Doom 的源码。

## 步骤

1. 构建主机端：`.\tools\build-host.ps1`，产物里有 `gl12\opengl32.dll` 和 `espgpu.dll`。
2. 准备 DSDA-Doom（Windows 版）和 `doom2.wad`。
3. 把 `tools/doom/dsda-doom.espgpu.cfg` 里的键合并进游戏的 cfg（关掉 VBO / FBO / 着色器 /
   模板等 GL 1.2 之外的路径，分辨率取设备的整数倍）。
4. 启动：

   ```powershell
   .\tools\doom\run-dsda.ps1 -DoomDir C:\games\dsda-doom -Iwad doom2.wad
   ```

   脚本会拷 DLL、设环境变量、`-warp 1 -skill 3` 进 MAP01。

### 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `ESPGPU_GL_MODE` | `espgpu` | `espgpu` / `trace`（同时记录每次调用）/ `passthrough`（转发给真实 GL） |
| `ESPGPU_GL_PASSTHROUGH_DLL` | `opengl32_mesa.dll`（exe 旁）→ 系统 `opengl32.dll` | 直通目标 |
| `ESPGPU_RES` | `320x200` | 设备分辨率（GL 窗口坐标会按比例缩放到它） |
| `ESPGPU_FPS` | `24` | 目标帧率 |
| `ESPGPU_GL_WAIT_MS` | 2 个帧周期 | SwapBuffers 等设备回帧的上限 |
| `ESPGPU_GL_LZ` | `1` | 下行命令流 LZ 压缩 |
| `ESPGPU_GL_AUTOPALETTE` | `1` | ≤256 色的 RGBA 纹理自动转 INDEX8 + 调色板 |
| `ESPGPU_GL_TRACE_FRAMES` | `3` | trace 模式下逐调用日志记录的帧数（`-1` = 一直记） |
| `ESPGPU_GL_TRACE_FILE` | `espgpu-gltrace.txt` | 调用清单输出路径 |
| `ESPGPU_GL_LOG` | `0` | `1` = 写 `espgpu-gl.log` |

设备打不开时，`espgpu` 模式会自动退回直通（日志里有一行 `switching to passthrough`），
游戏照常运行，便于先排除游戏侧问题。

## 先跑直通 + trace（PLAN R1，不可跳过）

真实的 GL 调用面要实测。用 Mesa llvmpipe 当直通目标最稳：

1. 下载 Mesa for Windows（如 pal1000/mesa-dist-win 的 x64 版），把它的 `opengl32.dll`
   改名成 `opengl32_mesa.dll` 放到游戏目录。
2. `.\tools\doom\run-dsda.ps1 -DoomDir ... -Mode passthrough`，正常玩一会儿 MAP01，退出。
3. 游戏目录下得到 `espgpu-gltrace.txt`：每个入口的调用次数 / 每帧次数、每个枚举参数见过的值、
   所有 `wglGetProcAddress` 请求。
4. 生成对照表：

   ```powershell
   python tools\doom\gen_coverage.py C:\games\dsda-doom\espgpu-gltrace.txt > docs\gl-coverage-trace.md
   ```

   表里「被调用但 espgpu 模式未实现」那一节就是要补的入口。

`trace` 模式（设备在线）还会写 `espgpu-glcalls.log`：前 3 帧每次调用带参数。

## 带宽预期

| 项 | 做法 | 每帧 |
|---|---|---|
| 顶点 | 位置 I16×3（每批次包围盒中心 + 自适应小数位）+ 纹理 I16×2 + RGB565，约 12 B/顶点 | ~29 KB @2400 顶点 |
| 合批 | 状态不变的连续 `glBegin/glEnd` 归并；4 顶点 strip / fan 直接当四边形（无索引） | 命令头开销可忽略 |
| 显示列表 | 第 2 次调用起整张列表上传为设备端缓冲，回放每批次只发 ~24 B | 静态几何趋近 0 |
| 纹理 | 自动调色板化 1 B/px；主机侧 LRU 镜像，只在换入时上传 | 关卡加载后接近 0 |
| 下行压缩 | CMD chunk 走 LZ4 block | 再省 30–50%（估计） |

上行帧回传按 M2 实测约 7–15 KB/帧。合计仍可能略超 35 KB/帧的预算，届时优先开显示列表、
降低 `ESPGPU_FPS` 到 20，或把设备分辨率降到 `256x160`。

## 已知缺口 / 风险

- **模板缓冲**：设备没有。依赖模板的天空 / 门户 / 镜子效果会缺失（`gl_use_stencil 0`）。
- **多重采样、各向异性、mipmap**：不支持。只用纹理层级 0，大于 512 的纹理在主机上缩小。
- **`BIND_TEXTURE` 只有一个 wrap**：取 s 方向。天空纹理（s 重复、t 夹取）顶部可能有接缝。
- **`glCopyTexSubImage2D`**：设备的 COPY 只能整张替换，偏移拷贝按「整张纹理对齐到源点」近似
  （DSDA 的 wipe 效果可能偏位）。
- **点光源**：近似成从眼点指向光源的方向光；不支持聚光、衰减、高光。
- **RGBA 纹理超过 256 色**：退回 RGBA4444，颜色只有 4 位精度（Doom 自身纹理不会触发）。
- **DSDA-Doom 新版本若强制 GL 2.0 / GLSL**：我们报告的是 `1.2`，会被拒绝。届时改用 prboom-plus
  2.6.x，或在 trace 里确认它要求的具体能力。
- **设备是 tile 延迟渲染**时，同一帧内删除 / 重传已被前面绘制引用的纹理需要设备先把分箱冲掉；
  主机侧 LRU 只在万不得已时才换出本帧用过的纹理。
- 每进程只能一个程序打开设备（WinUSB 独占）：游戏运行时 `espgpu-panel` 连不上设备。
