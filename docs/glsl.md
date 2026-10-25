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
| 预处理 | `#define`（对象式 / 函数式 / `##`）、`#undef`、`#if/#ifdef/#ifndef/#elif/#else/#endif`（`#if` 支持 `?:`、`&&`/`||` 短路）、`defined`、`#error`、`#extension`/`#pragma`/`#line`（忽略）、`GL_ES`、`__VERSION__`、`__LINE__`、LF / CRLF 续行；函数式宏的实参不能跨行 |
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

输出颜色的每个分量拆成乘法因子（纹理 / 顶点色 / 只依赖 uniform 与常量的 "uo" 因子），
四个分量的纹理和顶点色因子必须一致（设备按分量相乘），再和 `ESPGPU_SHADE_*` 对照：

| 着色器写法（任意顺序 / 经局部变量中转都可以） | 结果 |
|---|---|
| `o = vColor`、`o = vec4(vRGB, 1.0)`（3 分量 varying） | `FLAT`，varying → COLOR 槽（3 分量时设备补 alpha = 1） |
| `o = u`、`o = vec4(1.0, 0.5, 0.0, 1.0)`、`o = u * k` | `FLAT`，常量色在主机算好由 `COLOR` 命令下发 |
| `o = vColor * u`（任意 uo 因子） | `FLAT`，运行时把 uo 因子逐顶点乘进顶点色（插值是线性的，结果相同） |
| `o = texture(s, uv)` | `TEX`，uv → TEXCOORD0 |
| `o = texture(s, uv) * vColor` / `* vec4(vRGB, 1.0)` / `* u` / `* 0.5` / 三者组合 | `TEX_MOD_COLOR` |
| `o = texture(a, uv0) * texture(b, uv1) [* 顶点色] [* uo]` | `TEX_MOD_TEX`（没有颜色时常量白） |
| 以上任一 + `if (c.a OP k) discard;`（OP 为 `< <= > >=`，k 常量） | 再加 alpha 测试，参考值按"字节 b/255 与 k 比较"逐字节求等价的 `GEQUAL/LEQUAL ref` |

`* 1.0`、`+ 0.0` 之类在构建 DAG 时就被化简掉，不影响识别。`FragmentPlan::color_slots` 是 uo 因子
在常量表里的槽（见下）；纹理 alpha 被换掉（`vec4(tex.rgb, 1.0)`）或 4 分量顶点色的 alpha 被换掉时
设备没有对应模式，走 VM。

## 片元着色器：VM 降级

依次做：

1. 窥孔改写：`SUB(0, x)` 折进源取负修饰；`MUL` + `ADD/SUB` → `MAD`（含取负）；
   `x·m + a` 再与 uo 系数做加 / 乘 / 除时重结合成一条 `MAD`（新系数在主机上算）；
   `SEL(x < 0 / x >= 0 / 0 < x / 0 >= x, a, b)` → `CMP`；`discard` 条件是 `x < 0` / `0 < x` 时 KIL 直接用 x；
   `clamp(X, 0, 1)` → X 带 `_sat`；`pow(x, 1..4)` 换成乘法、标量 `inversesqrt` → `RSQ`。
2. 主机预计算（preshader）：只依赖 uniform / 常量的子表达式不发 VM 指令，每次绘制由
   `fragment_consts()` 在主机上算一遍（与 VM 同一份 `eval_op` 语义），结果放进 c 槽。
   例如 `sin(uTime * 0.3)`、`uRes.x / uRes.y`、`1.0 / (end - start)`。
3. SLP：同一个操作数里来自几个同构标量运算（同 op、只被这里用到）的分量合并成一条向量运算，
   自底向上递归（plasma 结尾的三组 `0.5 + 0.5 * sin(v + k)` 变成一条 ADD、一条 SIN、一条 MAD）。
4. 发射顺序：从各 KIL（越早丢弃越省）和输出颜色做后序 DFS，子树按 Sethi-Ullman 需求从大到小，
   比源码顺序少占很多寄存器。
5. 寄存器按分量分配：结果只占用到的分量（`TEX` 必须恒等分量）；同一个操作数要用到的几个结果
   尽量放进同一寄存器的不同分量，省掉拼装 MOV；只给输出颜色用的结果直接写进 o0 的对应分量；
   o0 初值 (0,0,0,1)，颜色里等于初值的常量分量（常见的 alpha = 1）不写。
6. 上面的预计算 / 打包 / SLP 各有代价（多占常量槽 / 临时寄存器），8 种开关组合各生成一遍，
   取指令最少的（同长取常量槽少的）。

- varying → v 寄存器：优先 TEXCOORD0..3（v1..v4）、NORMAL（v5），最后才用 COLOR（v0，设备可能夹取颜色）。
- 常量表 `c[0 .. frag.const_slots)`：先是 uniform 叶子（从 c0 起按 vec4 槽排列，matN 占 N 槽），
  其后是编译期常量与预计算结果；全 0 / 全 1 走特殊寄存器不占槽。合计不超过 32。
  运行时每次绘制调 `fragment_consts()` 生成整表，只下发与设备镜像不同的那一段。
- 采样器按首次使用顺序映射到设备纹理单元 0..3。
- 数值语义：逐分量运算全部来自 `shared/espgpu_vm_math.h`（`espgpu_vm_eval_scalar` / `espgpu_vm_dot`），
  `eval_op`（常量折叠 / 预计算 / 主机顶点着色器 / 参考 VM）和 `firmware/core/vm.c` 调用的是同一份代码，
  所以折叠与否、主机与设备结果逐位相同（同一编译器 / FP 收缩设置下；设备 GCC 会把 a*b+c 合成 MADD.S，
  跨平台最多差 1~2 ulp）。sin / cos / exp2 / log2 / pow 是多项式近似，不调 libm，见下文"设备上的 VM 执行"。
- 生成的代码从不读未写过的临时寄存器分量（参考解释器把 temps 初始化成 NaN 来验证这一点；
  设备规范是每像素清零，差分测试给固件传 NaN 填充的 worker 寄存器区，验证设备自己清零）。

`gl_FragCoord` 依赖隐藏 uniform `@fragcoord = (kx, ky, bx, by)`，`xy = v6.xy * (kx, ky) + (bx, by)` 一条 MAD：
设备 v6 是设备像素、左上原点、像素中心 +0.5；屏幕目标 `kx = 窗口宽/设备宽`、`ky = -窗口高/设备高`、
`bx = 0`、`by = 窗口高`，FBO 目标 `(1, 1, 0, 0)`，得到 GL 语义的窗口像素、左下原点坐标。
它不出现在 `glGetActiveUniform` / `glGetUniformLocation` 里。

参考指令数（不含 END）：

| 着色器 | 原来 | 现在 |
|---|---|---|
| demo plasma（`host/gles3/demo/shader.cpp`） | 45 | 23（+10 个主机预计算运算） |
| demo 卡片（暗角 + 圆形 discard） | 12 | 10 |
| 纹理 + 线性雾 `mix(fog, tex, clamp((end-d)/(end-start), 0, 1))` | 7 | 3 |
| 屏幕空间圆 `1 - smoothstep(r-1, r+1, length(gl_FragCoord.xy - c))` | 14 | 5 |
| Lambert（normalize + dot + 纹理） | 10 | 9 |
| Phong（reflect + pow） | 19 | 16 |
| 4 个点光源循环 | 48 | 42 |
| `atan` 旋涡 | 33 | 26 |
| `vec4(texture(t, uv).rgb, 1.0)` | 3 | 1 |
| 常量色 / `vec4(vRGB, 1.0)` / `vColor * uTint` / `texture * 0.5` / `texture * vec4(vRGB, 1.0)` | 1–4 | 0（内置模式） |

## 设备上的 VM 执行（`firmware/core/vm.c`）

`LOAD_PROGRAM` 时 `gp_vm_validate` 校验之后把程序预解码成标量微指令（uop），执行期不再看原始字节码：

- 每条向量指令按写掩码拆成逐分量的标量 uop，掩码外的分量不计算（原来 `sin r0.x` 也算 4 个 sin）；
  `DP2/3/4` 拆成 `MUL` + `MAD` 链（与 `espgpu_vm_dot` 同一舍入顺序），`RCP/RSQ` 算一次再复制；
  同一条指令里先写后读的分量走暂存格，保证向量语义。
- 操作数是一张 ≤128 格的标量表 `S[]` 的字节下标：用到的 temp 分量、o0、常量分量、varying 分量，
  外加 0 / 1 / -1 / -0；取负的常量 / varying 在每像素拷入时取负，只有取负的 temp 需要通用 uop。
- 复制传播、公共子表达式（`vec3(x)` 广播、重复的 `mul`）、死代码删除；只对"先读后写"的 temp 分量每像素清零。
- 执行：一个 `switch`（覆盖全部操作码、default 不可达，省掉范围检查），END 哨兵结束。
- 解码结果存在 `vm.c` 私有表里、以 `gp_prog_t.code` 的地址为键（core.c 每次 LOAD_PROGRAM 都先 `gp_flush`
  再校验，所以不会有 worker 同时在跑它）。表满 / S[] 放不下 / 内存不足时走逐条解释的后备路径，结果逐位相同。
- `gp_vm_run_span()`：一段 ≤8 个片元一起执行（段内 varying 线性，传起点和步长），每条 uop 对 8 个像素循环，
  取指 / 分派开销摊薄 8 倍；结果与逐像素 `gp_vm_run` 逐位相同。需要 raster3d 的 VM 像素循环改成按段调用才生效。

数学函数（`shared/espgpu_vm_math.h`，只用加 / 乘 / madd、float↔int 转换和整数位运算）：

| 函数 | 方法 | 精度（对双精度 libm） |
|---|---|---|
| sin / cos | `n = round(x/π)`（1.5·2^23 魔数），Cody–Waite 三段 π/2 约简（\|x\| ≤ 2.05e5），奇 9 次 minimax；更大的 \|x\| 用 1/π 的 256 位常数做 Payne–Hanek | 绝对误差 ≤ 1.3e-7（\|x\| ≤ 1e4），任意有限输入 ≤ 1e-6；`sin(±π/2) = ±1`、`cos(0) = 1` |
| exp2 | 指数加到位上 + `[0,1)` 上 5 次 minimax | 相对 ≤ 2e-7 |
| log2 | 尾数规约到 `[√½, √2)`，`t·R(t)` 8 次 | ≤ 2e-7·max(1, \|log2 x\|) |
| pow | `exp2(b·log2 a)` | 相对约 (1 + \|b·log2 a\|)·2e-7 |
| floor / fract / mod | 截断转换 + 修正（\|x\| ≥ 2^23 原样） | 精确 |
| div / rcp / sqrt / rsq | IEEE 运算（本来就正确舍入、跨平台一致） | 精确 |

边界语义（ISA 的一部分，头文件注释是权威版本）：`sin/cos(±inf, NaN) = NaN`；`exp2(x ≥ 128) = +inf`、
`exp2(x < -126) = 0`；`log2(x) = -126`（x < 2^-126、≤0 或 NaN），`log2(+inf) = +inf`；`pow(a, 0) = pow(1, b) = 1`，
`a ≤ 0` 或 NaN 按 0（b > 0 得 0，b < 0 得 +inf）；`floor(-0) = +0`；除零得 0；`sqrt(a) = sqrt(|a|)`；`rsq(0) = 0`。
FMA 求值时 sin 在 π/2 附近可能比 1 大 1 ulp。

每像素开销（plasma，23 条 VM 指令 / 31 个 uop，其中 7 个 sin 分量；本机 arm64 实测退休指令数）：
原来约 3850 条，预解码后约 1030 条，整段执行约 810 条。设备上原来每像素 20 次 newlib `sinf`（各数百周期，
掩码外的分量也算），现在 7 个内联 sin，各约 40 条指令。

## 顶点着色器

在 AST 上识别：`main` 里只有

- 局部变量声明（初始化表达式会被代入）
- `gl_Position = U0 * U1 * … * P`，其中 `Ui` 是 `uniform mat4`，`P` 是 vec4 属性、
  `vec4(a.xyz, 1.0)` 或 `vec4(a.xy, 0.0, 1.0)`（正好是设备的分量补齐规则）
- `vOut = attr` / `vOut = attr.xy…`（属性直通）

命中则走设备 T&L：运行时在主机上把 `U0·U1·…` 相乘（列主序）转成行主序下发
`ESPGPU_MATRIX_MVP`，属性按源格式原样转发（I8/U8/I16/U16/I32/U32/F32/FIXED，归一化标志照搬；
半精度或分量不足且设备缺省值与 GL 不同时转 F32）。否则在主机上批量解释执行 DAG
（`run_vertex_batch`：只依赖 uniform 的子表达式整批只算一次），发送裁剪坐标 + varying，MVP 置单位阵；
整批 w 都是 1 时位置只发 xyz，z 也都是 0 时只发 xy。

## GLES 运行时要点

- 进程内只开一个设备；`ESPGPU_RES=WxH`（默认 320x200；`gles-shader` demo 没设时用 160x100）决定设备分辨率，
  GL 视口 / 裁剪框按窗口尺寸缩放并换成设备的左上原点；FBO 目标保持左下原点。
- 所有设备状态（管线、布局、矩阵、纹理单元、常量寄存器、渲染目标、视口）都有镜像，只发变化。
- 片元程序缓存设备的 16 个程序槽（LRU）。
- 纹理：主机保留 RGBA8 母本，上传时转成 RGB565 / RGBA4444 / R8，超过设备上限最近邻缩小；
  主机侧 LRU 镜像按设备纹理缓存预算的 7/8 主动换出，设备的 `TEXTURE_EVICTED` 事件触发重传。
- 顶点流：内置模式的顶点色在 [0,1] 内时按 U8 归一化发送（16 → 4 字节/顶点；设备最终也按 8 位用）；
  索引很稀疏时（大 VBO 里画一小段）只发被引用的顶点并重排索引。
- `eglSwapBuffers` = `PRESENT` + 等待该帧（有上限）+ `espgpu_blit_to_hdc`。
- PBO（`PIXEL_UNPACK/PACK_BUFFER`）读写范围越界报 `GL_INVALID_OPERATION`。
- 线程：设备镜像与对象表都没有加锁，同一时刻只能有一个线程在调 GL。
- `glReadPixels` 返回最近一次呈现的帧（设备只在 PRESENT 时回传）。
- 模板测试、MSAA、多渲染目标、实例化（count≠1）、压缩纹理、cube/3D/数组纹理：接受调用但不生效，
  首次调用记一条日志。

## 性能注记

- VM 每像素逐条解释 float 运算，大约是内置模式的 1/2 到 1/5 速度（PLAN §6 估"性能减半"）。
  issue #2 实测原实现 320x200 plasma 一帧 1.4 s；预解码 + 内联 sin 后估计每像素 1.3k–1.8k 周期
  （原来约 10k），320x200 约 0.2–0.3 s、160x100 约 50–70 ms；raster3d 改用 `gp_vm_run_span` 后再快约 30%。
  所以 `gles-shader` 默认把设备开成 160x100（标题栏 / 控制台打印分辨率和帧率），`ESPGPU_RES=320x200` 看全分辨率。
- 优先写成内置模式能识别的形式：纹理 × 顶点色 × 常量、纹理 × 纹理、alpha 丢弃。
- 只依赖 uniform 的计算放在着色器里也不花 VM 指令（主机预计算），不必手工挪到 CPU。
- 主机解释执行的顶点着色器对 Doom 级别的顶点量（几千/帧）开销可以忽略。

## 测试

```bash
clang++ -std=c++20 -fsanitize=address,undefined -Ihost/glsl/include -Ihost/glsl/src -Ishared \
    host/glsl/src/*.cpp host/glsl/tests/glsl_tests.cpp -o glsl-tests && ./glsl-tests
```

与固件 VM 做差分（把 `firmware/core/*.c` 一起链接进来）：

```bash
for f in firmware/core/*.c; do clang -std=c11 -c -Ishared -Ifirmware/core $f -o /tmp/$(basename $f .c).o; done
clang++ -std=c++20 -fsanitize=address,undefined -DGLSL_TESTS_FIRMWARE_VM -Ihost/glsl/include -Ihost/glsl/src \
    -Ishared -Ifirmware/core host/glsl/src/*.cpp host/glsl/tests/glsl_tests.cpp /tmp/*.o -o glsl-tests && ./glsl-tests
```

CMake 目标 `glsl-tests` 同样可在 Windows 上构建运行（有 `espgpu_core` 时自动带固件差分）。覆盖预处理、
语法 / 类型错误（含行号）、模式识别与 alpha 参考值、VM 执行（参考解释器 `vm_run` 与手算值对比）、
差分测试（31 个着色器 × 200 组随机输入，C++ 直接计算 vs 参考 VM vs 固件 VM，8 种优化开关组合各跑一遍；
参考 VM、固件预解码、固件后备解释器三者逐位比较）、顶点着色器识别与主机（批量）执行、链接；
`espgpu_vm_math.h` 对 libm 的稠密精度扫描（含 |x| 到 FLT_MAX 的 sin/cos、整个正规数范围的 log2）与边界语义；
随机 VM 字节码（随机操作码 / 写掩码 / swizzle / 取负 / _sat / KIL / 特殊值输入）在参考 VM、固件预解码、
固件后备、`gp_vm_run_span` 之间逐位比较。

GLES 运行时端到端（本机，不需要 Windows / 硬件；Win32 与 espgpu.dll 用 `host/gles3/tests/native_shim` 的替身，
命令直接交给固件核心光栅，再用 `glReadPixels` 检查像素：方向、gl_FragCoord、常量色 / 顶点色缩放、U8 / I8 属性
原样转发、alpha 丢弃、FBO 渲染到纹理再采样、VM 预计算、稀疏索引的下行字节数、PBO 越界、无表面 MakeCurrent）：

```bash
for f in firmware/core/*.c; do clang -std=c11 -fsanitize=address,undefined -c -Ishared -Ifirmware/core $f -o /tmp/$(basename $f .c).o; done
clang++ -std=c++20 -fsanitize=address,undefined -DESPGPU_GLES_BUILD -Ihost/gles3/tests/native_shim -Ihost/gles3/include \
    -Ihost/gles3/src -Ihost/include -Ihost/glsl/include -Ihost/glsl/src -Ishared -Ifirmware/core \
    host/gles3/tests/gles_e2e.cpp host/gles3/tests/native_shim/espgpu_shim.cpp host/gles3/src/gl_objects.cpp \
    host/gles3/src/gl_draw.cpp host/gles3/src/bridge.cpp host/glsl/src/*.cpp /tmp/*.o -o gles-e2e && ./gles-e2e
```
