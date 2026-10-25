# GLSL ES 编译器与 GLES 3.0 前端（M6）

代码：`host/glsl/`（编译器，纯 C++20 静态库，平台无关）、`host/gles3/`（`libGLESv2.dll` + `libEGL.dll` + demo）。
指令集定义：`shared/espgpu_vm.h`。

## 总体流程

```
GLSL 源码 ─ 预处理 ─ 词法 ─ 递归下降语法分析 ─ 符号执行（类型检查/折叠/内联/展开/谓词化）
                                                        │
                                              无分支 DAG（标量引用 → 节点）
                                     ┌──────────────────┴───────────────────┐
                                片元着色器                               顶点着色器
                          模式识别 ── 命中 → 内置 shade_mode       AST 上识别 gl_Position = U·…·pos
                              └─ 未命中 → VM 字节码 (≤256 条)        ├─ 命中 → 设备 T&L（下发 MVP）
                                                                    └─ 否则 → 主机逐顶点解释执行 DAG
```

符号执行把每个变量表示成一串"标量引用"（DAG 节点 + 分量）。这样 swizzle 写、
结构体、数组都只是改引用；if/else 不需要分支——两侧都执行，赋值写成
`SEL(谓词, 新值, 旧值)`；return / break / continue 把当前谓词累加进退出集合；
循环在谓词折叠成常量 0 之前持续展开（for 初始化里声明的计数器写入不加谓词，
所以总能折叠）。

## 支持的 GLSL 子集

| 类别 | 支持 |
|---|---|
| 版本 | `#version 100`、`#version 300 es` |
| 预处理 | `#define`（对象式 / 函数式 / `##`）、`#undef`、`#if/#ifdef/#ifndef/#elif/#else/#endif`、`defined`、`#error`、`#extension`/`#pragma`/`#line`（忽略）、`GL_ES`、`__VERSION__`、`__LINE__` |
| 类型 | `float int uint bool`、`vec/ivec/uvec/bvec 2-4`、`mat2-4`、`matCxR`、`sampler2D`、结构体、常量长度一维数组 |
| 限定符 | `in out inout uniform const`、`attribute varying`（1.00）、`layout(location=N)`；精度 / `flat` / `centroid` / `invariant` 解析后忽略 |
| 语句 | 块、声明、表达式、`if/else`、`for/while/do`（常量次数）、`break/continue/return/discard` |
| 内建函数 | 三角（含 `atan(y,x)`/`asin`/`acos` 多项式近似）、指数对数、`pow sqrt inversesqrt abs sign floor ceil trunc round fract mod min max clamp mix step smoothstep`、几何（`length distance dot cross normalize reflect refract faceforward`）、矩阵（`matrixCompMult outerProduct transpose determinant`）、向量比较、`any all not`、`texture texture2D textureProj textureLod`（lod 忽略） |
| 内建变量 | `gl_Position gl_PointSize`（VS）、`gl_FragColor gl_FragData[0] gl_FragCoord gl_FrontFacing(恒真) gl_PointCoord(常量)`（FS） |

**不支持**（编译期报错，带行列号）：`switch`、uniform block、非 `sampler2D` 采样器、
采样器数组动态下标、动态次数循环、递归、`dFdx/dFdy/fwidth`、`texelFetch/textureSize`、
`inverse`、位运算（仅常量表达式可用）、顶点着色器里采样纹理。

GLSL ES 没有隐式类型转换，编译器严格按此检查（`1.0 + 1` 报错）。

## 片元着色器：模式识别

DAG 输出颜色被拆成乘法因子，再和 `ESPGPU_SHADE_*` 对照：

| 着色器写法（任意顺序 / 经局部变量中转都可以） | 结果 |
|---|---|
| `o = vColor` | `FLAT`，vColor → COLOR 槽 |
| `o = u`（uniform vec4） | `FLAT`，常量色由 `COLOR` 命令下发 |
| `o = texture(s, uv)` | `TEX`，uv → TEXCOORD0 |
| `o = texture(s, uv) * vColor` 或 `* u` | `TEX_MOD_COLOR` |
| `o = texture(a, uv0) * texture(b, uv1) [* vColor]` | `TEX_MOD_TEX`（没有颜色时常量白） |
| 以上任一 + `if (c.a < k) discard;` | 再加 alpha 测试 `GEQUAL ref=round(k·255)`（`<=` → `GREATER`） |

`* 1.0`、`+ 0.0` 之类在构建 DAG 时就被化简掉，不影响识别。

## 片元着色器：VM 降级

- 寄存器：节点按 id（即拓扑序）线性扫描分配 r0..r15，超过报错。
- varying → v 寄存器：优先 TEXCOORD0..3（v1..v4）、NORMAL（v5），最后才用 COLOR（v0，设备可能夹取颜色）。
- uniform：每个叶子从 c0 起按 vec4 槽排列（matN 占 N 槽），常量打包进其后的槽，
  全 0 / 全 1 走特殊寄存器不占槽。合计不超过 32。
- 采样器按首次使用顺序映射到设备纹理单元 0..3。
- 窥孔：只被一个 ADD 使用的 MUL 合并成 MAD；最终颜色需要拼装时直接拼进 o0。
- 生成的代码从不读未写过的临时寄存器（设备在像素间不清零 temps；参考解释器用
  NaN 初始化来验证这一点）。

`gl_FragCoord` 依赖隐藏 uniform `@fragcoord = (sx, sy, oy, fy)`：设备 v6 是设备像素、
左上原点，运行时每次绘制写入换算系数，得到 GL 语义的窗口像素、左下原点坐标。
它不出现在 `glGetActiveUniform` / `glGetUniformLocation` 里。

参考：一个 4 层 sin 叠加的 plasma（`host/gles3/demo/shader.cpp`）编译成 46 条指令。

## 顶点着色器

在 AST 上识别：`main` 里只有

- 局部变量声明（初始化表达式会被代入）
- `gl_Position = U0 * U1 * … * P`，其中 `Ui` 是 `uniform mat4`，`P` 是 vec4 属性、
  `vec4(a.xyz, 1.0)` 或 `vec4(a.xy, 0.0, 1.0)`（正好是设备的分量补齐规则）
- `vOut = attr` / `vOut = attr.xy…`（属性直通）

命中则走设备 T&L：运行时在主机上把 `U0·U1·…` 相乘（列主序）转成行主序下发
`ESPGPU_MATRIX_MVP`，属性原样作为 F32 发送。否则在主机上逐顶点解释执行 DAG，
发送裁剪坐标 + varying，MVP 置单位阵。

## GLES 运行时要点

- 进程内只开一个设备；`ESPGPU_RES=WxH`（默认 320x200）决定设备分辨率，
  GL 视口 / 裁剪框按窗口尺寸缩放并换成设备的左上原点；FBO 目标保持左下原点。
- 所有设备状态（管线、布局、矩阵、纹理单元、常量寄存器、渲染目标、视口）都有镜像，只发变化。
- 片元程序缓存设备的 16 个程序槽（LRU）。
- 纹理：主机保留 RGBA8 母本，上传时转成 RGB565 / RGBA4444 / R8，超过设备上限最近邻缩小；
  主机侧 LRU 镜像按设备纹理缓存预算的 7/8 主动换出，设备的 `TEXTURE_EVICTED` 事件触发重传。
- `eglSwapBuffers` = `PRESENT` + 等待该帧（有上限）+ `espgpu_blit_to_hdc`。
- `glReadPixels` 返回最近一次呈现的帧（设备只在 PRESENT 时回传）。
- 模板测试、MSAA、多渲染目标、实例化（count≠1）、压缩纹理、cube/3D/数组纹理：接受调用但不生效，
  首次调用记一条日志。

## 性能注记

- VM 每像素逐条解释 float 运算，大约是内置模式的 1/2 到 1/5 速度（PLAN §6 估"性能减半"），
  320x200 全屏 46 条指令的 plasma 需要在设备上实测；超预算时可降 `ESPGPU_RES`。
- 优先写成内置模式能识别的形式：纹理 × 顶点色、纹理 × 纹理、alpha 丢弃。
- 主机解释执行的顶点着色器对 Doom 级别的顶点量（几千/帧）开销可以忽略。

## 测试

```bash
clang++ -std=c++20 -fsanitize=address,undefined -Ihost/glsl/include -Ihost/glsl/src -Ishared \
    host/glsl/src/*.cpp host/glsl/tests/glsl_tests.cpp -o glsl-tests && ./glsl-tests
```

CMake 目标 `glsl-tests` 同样可在 Windows 上构建运行。覆盖预处理、语法 / 类型错误（含行号）、
模式识别、VM 执行（参考解释器 `vm_run` 与手算值对比）、顶点着色器识别与主机执行、链接。
