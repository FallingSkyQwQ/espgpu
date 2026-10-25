# GL 1.2 前端覆盖情况（`host/gl12` → `opengl32.dll`）

> 逐入口的机器可读状态见 `host/gl12/gen/coverage.tsv`（由 `tools/gen_gl.py` 生成）。
> 实测调用清单（DSDA-Doom 真实调用了哪些）需要按 [doom.md](doom.md) 跑一次直通 + trace，
> 再用 `tools/doom/gen_coverage.py` 生成 `docs/gl-coverage-trace.md`。**本页下半部分的
> 「DSDA-Doom 预期调用」是按 glboom-plus 渲染器源码结构整理的预期，尚未经 trace 证实。**

## 总数

| 类别 | 入口数 | espgpu 模式已实现 | 桩 |
|---|---:|---:|---:|
| GL 1.1 核心（全部导出） | 336 | 304 | 32 |
| 扩展 / 1.2–1.3（经 `wglGetProcAddress`） | 108 | 108 | 0 |
| WGL（导出） | 25 | 25 | — |
| WGL 扩展 | 8 | 8 | — |

32 个桩全部是求值器（`glMap*` / `glEval*` / `glMapGrid*` / `glGetMap*`）、像素映射表
（`glPixelMap*` / `glGetPixelMap*`）和 `glGetTexGen*`。调用它们只会在日志里记一次
`unimplemented`，返回 0。

直通模式下所有 336 + 108 个入口都转发给真实 GL，并计数。

## 能力与扩展

`GL_VERSION` = `1.2 espgpu`，`GL_EXTENSIONS`：

| 扩展 | 实现方式 |
|---|---|
| `GL_ARB_multitexture` | 2 个纹理单元，折成 `ESPGPU_SHADE_TEX_MOD_TEX` / `TEX_ADD_TEX` |
| `GL_ARB_texture_env_combine` / `GL_EXT_texture_env_combine` | 组合表达式折成最接近的基本模式（REPLACE / MODULATE / ADD / DECAL / 透传），`RGB_SCALE` 忽略 |
| `GL_ARB_texture_env_add` / `GL_EXT_texture_env_add` | `ESPGPU_SHADE_TEX_ADD` |
| `GL_EXT_paletted_texture` / `GL_EXT_shared_texture_palette` | INDEX8 + `TEX_PALETTE`（每纹理或共享调色板） |
| `GL_ARB_texture_non_power_of_two` | 设备原生支持任意尺寸 |
| `GL_EXT_bgra`、`GL_EXT_packed_pixels` | 上传时转换 |
| `GL_EXT_texture_edge_clamp` / `GL_SGIS_texture_edge_clamp`、`GL_ARB_texture_mirrored_repeat` | CLAMP / MIRROR |
| `GL_EXT_texture_object`、`GL_EXT_vertex_array`、`GL_EXT_compiled_vertex_array`、`GL_EXT_draw_range_elements` | 别名到核心实现 |
| `GL_EXT_blend_minmax` / `blend_subtract` / `blend_color` | 混合方程映射到 `blend_op`；常量色因子近似为 ONE / ZERO |
| `GL_ARB_transpose_matrix` | 支持 |
| `GL_EXT_texture_filter_anisotropic` | **检测到也不报告**（硬件没有，PLAN §6 要求禁用） |

不报告：VBO、FBO、PBO、着色器、压缩纹理、多重采样、模板。

## 状态映射

| GL | 设备 |
|---|---|
| 矩阵栈（modelview 32 / projection 4 / texture 4） | MVP = P·MV·T(批次原点) 发给 `SET_MATRIX`；纹理矩阵在主机上乘掉 |
| `glBegin`/`glEnd`、顶点数组、`glDrawElements`、`glInterleavedArrays`、`glRect*` | 统一进待发批次；状态不变就一直合批 |
| `GL_QUADS` / 4 顶点 strip / fan / polygon | `ESPGPU_PRIM_QUADS`（无索引） |
| 其余三角形图元 | `TRIANGLES` + u16 索引 |
| 线 / 线带 / 线环、点 | `LINES` + 索引 / `POINTS`，宽度按窗口→设备比例缩放 |
| 显示列表 | 闭包 + 几何批次；第 2 次回放起几何上传为设备缓冲（`DRAW` + `VSRC_BUFFER`） |
| blend / alpha test / depth / cull / front face / shade model / polygon offset | `PIPELINE` + `pipeline_ext`（深度偏移只能表达负偏移） |
| fog（LINEAR / EXP / EXP2） | `PIPELINE` + `pipeline_fog`，设备用 clip w 作眼空间深度 |
| 光照（≤ 4 盏，GL_COLOR_MATERIAL） | `LIGHTING` + `SET_MATRIX(NORMAL)`；点光源近似成方向光 |
| 视口 / 裁剪（左下原点、窗口像素） | 按 窗口→设备 比例换算成左上原点 |
| `glClear` | `CLEAR`（受裁剪与 color/depth mask 约束） |
| `glCopyTexImage2D` / `glCopyTexSubImage2D` | `FBO COPY_TO_TEXTURE`（整张替换） |
| `glReadPixels` | PRESENT + 取回最近一帧，缩放到窗口坐标，按 pack 状态打包 |
| `glDrawPixels` | 临时纹理 + 屏幕对齐四边形 |
| `glFinish` / `glFlush` | `FENCE` + 等待 / 立即发出 |
| stencil / accum / logic op / clip plane / texgen / 选择 / 反馈 | 接受参数、保存状态、不生效 |

纹理格式：ALPHA → RGBA4444（rgb=1），LUMINANCE → R8，LUMINANCE_ALPHA / INTENSITY → RGBA4444，
RGB(A) → ≤256 色时 INDEX8 + 调色板，否则 RGB565 / RGBA4444，COLOR_INDEX → INDEX8。

## DSDA-Doom（glboom-plus 渲染器）预期调用 —— 待 trace 证实

| 用途 | 入口 | 状态 |
|---|---|---|
| 墙 / 地板 / 精灵 | `glBegin`(TRIANGLE_STRIP / TRIANGLE_FAN / QUADS)、`glVertex3f`、`glTexCoord2f`、`glColor4f`、`glColor3f` | ✅ |
| 顶点数组路径 | `glVertexPointer`、`glTexCoordPointer`、`glColorPointer`、`glEnableClientState`、`glDrawArrays`、`glDrawElements` | ✅ |
| 纹理 | `glGenTextures`、`glBindTexture`、`glTexImage2D`、`glTexSubImage2D`、`glTexParameteri/f`、`glDeleteTextures`、`glPixelStorei` | ✅ |
| 纹理尺寸探测 | `glTexImage2D(GL_PROXY_TEXTURE_2D)` + `glGetTexLevelParameteriv` | ✅（> 512 报 0） |
| 调色板纹理 | `glColorTableEXT` | ✅ |
| 多纹理（细节纹理） | `glActiveTextureARB`、`glClientActiveTextureARB`、`glMultiTexCoord2fARB`、`glTexEnvi`（含 COMBINE） | ✅（折叠近似） |
| 状态 | `glEnable/glDisable`（TEXTURE_2D、BLEND、ALPHA_TEST、DEPTH_TEST、FOG、CULL_FACE、SCISSOR_TEST、POLYGON_OFFSET_FILL）、`glAlphaFunc`、`glBlendFunc`、`glDepthFunc`、`glDepthMask`、`glDepthRange`、`glShadeModel`、`glHint`、`glColorMask`、`glPolygonOffset` | ✅ |
| 雾 | `glFogi`、`glFogf`、`glFogfv` | ✅ |
| 矩阵 | `glMatrixMode`、`glLoadIdentity`、`glLoadMatrixf`、`glMultMatrixf`、`glPushMatrix`、`glPopMatrix`、`glTranslatef`、`glRotatef`、`glScalef`、`glOrtho`、`glFrustum` | ✅ |
| 帧 | `glViewport`、`glScissor`、`glClear`、`glClearColor`、`glClearDepth`、`glFinish`、`glFlush`、`SwapBuffers` | ✅ |
| 截图 / wipe | `glReadPixels`、`glCopyTexSubImage2D` | ⚠️ 近似 |
| 天空 / 门户 | `glStencilFunc`、`glStencilOp`、`glClipPlane` | ⚠️ 不生效 |
| 自动图 | `GL_LINES`、`glLineWidth` | ✅ |
| 查询 | `glGetString`、`glGetIntegerv`(MAX_TEXTURE_SIZE / MAX_TEXTURE_UNITS / VIEWPORT …)、`glGetFloatv`、`glGetError`、`wglGetProcAddress` | ✅ |
| VBO / FBO / 着色器 | `glGenBuffersARB`、`glGenFramebuffersEXT`、`glCreateShaderObjectARB` … | ❌ 不报告扩展，游戏应走回退路径 |
