/*
 * gles_e2e.cpp — GLES 运行时端到端测试（本机构建，不需要 Windows / 硬件）
 *
 * GL 调用 → libGLESv2 源码（gl_objects / gl_draw / bridge）→ 命令直接交给固件核心
 * （firmware/core，与模拟器同一份 C 代码）→ PRESENT 的像素 → glReadPixels 检查。
 * Win32 与 espgpu.dll 用 native_shim/ 下的替身。构建见 docs/glsl.md「测试」。
 */
#define GL_APICALL
#include <GLES3/gl3.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
int espgpu_gles_initialize(void);
void *espgpu_gles_create_context(void *share, int version);
int espgpu_gles_make_current(void *ctx, void *hwnd, int w, int h);
void espgpu_gles_swap(void);
size_t shim_bytes();
void shim_reset_bytes();
}

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static GLuint prog(const char *vs, const char *fs)
{
    auto sh = [](GLenum t, const char *s) {
        GLuint x = glCreateShader(t);
        glShaderSource(x, 1, &s, nullptr);
        glCompileShader(x);
        GLint ok;
        glGetShaderiv(x, GL_COMPILE_STATUS, &ok);
        char log[1024];
        glGetShaderInfoLog(x, sizeof(log), nullptr, log);
        if (!ok) std::printf("compile: %s\n", log);
        return x;
    };
    GLuint p = glCreateProgram();
    glAttachShader(p, sh(GL_VERTEX_SHADER, vs));
    glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::printf("link: %s\n", log);
    }
    return p;
}

static void px(int x, int y, unsigned char o[4]) { glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, o); }
static bool close_to(const unsigned char *p, int r, int g, int b, int tol = 12)
{
    const bool ok = std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol;
    if (!ok) std::printf("  pixel (%d %d %d) want (%d %d %d)\n", p[0], p[1], p[2], r, g, b);
    return ok;
}

static const char *kVSPosCol = "#version 300 es\nin vec2 aPos; in vec4 aCol; out vec4 vCol;\nvoid main(){ vCol = aCol; gl_Position = vec4(aPos, 0.0, 1.0); }";

int main()
{
    if (!espgpu_gles_initialize()) { std::printf("init failed\n"); return 1; }
    void *ctx = espgpu_gles_create_context(nullptr, 3);
    espgpu_gles_make_current(ctx, nullptr, 320, 200);
    unsigned char p[4];

    /* 1. 顶点色 + 方向：上半屏绿、下半屏红（GL 左下原点） */
    {
        GLuint pr = prog(kVSPosCol, "#version 300 es\nprecision mediump float;\nin vec4 vCol; out vec4 o;\nvoid main(){ o = vCol; }");
        glUseProgram(pr);
        const float pos[] = {-1, -1, 1, -1, 1, 0, -1, -1, 1, 0, -1, 0, -1, 0, 1, 0, 1, 1, -1, 0, 1, 1, -1, 1};
        float col[24 * 2];
        for (int i = 0; i < 12; i++) {
            const bool top = i >= 6;
            col[i * 4 + 0] = top ? 0 : 1;
            col[i * 4 + 1] = top ? 1 : 0;
            col[i * 4 + 2] = 0;
            col[i * 4 + 3] = 1;
        }
        const GLint lp = glGetAttribLocation(pr, "aPos"), lc = glGetAttribLocation(pr, "aCol");
        glEnableVertexAttribArray(lp);
        glEnableVertexAttribArray(lc);
        glVertexAttribPointer(lp, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 0, col);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        shim_reset_bytes();
        glDrawArrays(GL_TRIANGLES, 0, 12);
        std::printf("  draw 1 (12 verts, F32 colours -> U8): %zu bytes\n", shim_bytes());
        espgpu_gles_swap();
        px(160, 40, p);
        CHECK(close_to(p, 255, 0, 0));
        px(160, 160, p);
        CHECK(close_to(p, 0, 255, 0));
        glDisableVertexAttribArray(lc);
    }
    /* 2. 常量色（内置 FLAT + COLOR 命令） */
    {
        GLuint pr = prog("#version 300 es\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }",
                         "#version 300 es\nprecision mediump float;\nout vec4 o;\nvoid main(){ o = vec4(0.0, 0.0, 1.0, 1.0); }");
        glUseProgram(pr);
        const float pos[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        espgpu_gles_swap();
        px(100, 100, p);
        CHECK(close_to(p, 0, 0, 255));
    }
    /* 3. 顶点色 × uniform（运行时逐顶点乘） + U8 顶点色原样转发（T&L） */
    {
        GLuint pr = prog("#version 300 es\nuniform mat4 uM;\nin vec2 aPos; in vec4 aCol; out vec4 vCol;\nvoid main(){ vCol = aCol; gl_Position = uM * vec4(aPos, 0.0, 1.0); }",
                         "#version 300 es\nprecision mediump float;\nuniform vec4 uTint; in vec4 vCol; out vec4 o;\nvoid main(){ o = vCol * uTint; }");
        glUseProgram(pr);
        const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        glUniformMatrix4fv(glGetUniformLocation(pr, "uM"), 1, GL_FALSE, id);
        glUniform4f(glGetUniformLocation(pr, "uTint"), 0.5f, 0.25f, 1.0f, 1.0f);
        const signed char pos[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
        const unsigned char col[] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                     255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
        const GLint lp = glGetAttribLocation(pr, "aPos"), lc = glGetAttribLocation(pr, "aCol");
        glEnableVertexAttribArray(lp);
        glEnableVertexAttribArray(lc);
        glVertexAttribPointer(lp, 2, GL_BYTE, GL_FALSE, 0, pos);
        glVertexAttribPointer(lc, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, col);
        shim_reset_bytes();
        glDrawArrays(GL_TRIANGLES, 0, 6);
        std::printf("  draw 3 (6 verts, T&L, I8 pos raw + scaled U8 colour): %zu bytes\n", shim_bytes());
        espgpu_gles_swap();
        px(100, 100, p);
        CHECK(close_to(p, 128, 64, 255));
        /* 不缩放时 U8 颜色原样转发 */
        glUniform4f(glGetUniformLocation(pr, "uTint"), 1, 1, 1, 1);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        espgpu_gles_swap();
        px(100, 100, p);
        CHECK(close_to(p, 255, 255, 255));
        glDisableVertexAttribArray(lc);
    }
    /* 4. gl_FragCoord（左下原点、像素中心 .5） */
    {
        GLuint pr = prog("#version 300 es\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }",
                         "#version 300 es\nprecision highp float;\nout vec4 o;\nvoid main(){ o = vec4(gl_FragCoord.x / 320.0, gl_FragCoord.y / 200.0, gl_FragCoord.y < 100.0 ? 1.0 : 0.0, 1.0); }");
        glUseProgram(pr);
        const float pos[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        espgpu_gles_swap();
        px(40, 20, p);
        CHECK(close_to(p, int(40.5 / 320 * 255), int(20.5 / 200 * 255), 255));
        px(280, 180, p);
        CHECK(close_to(p, int(280.5 / 320 * 255), int(180.5 / 200 * 255), 0));
    }
    /* 5. 纹理 + alpha 丢弃（内置 TEX + alpha 测试）+ FBO 渲染到纹理再采样（方向） */
    {
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        /* 2x2：第 0 行（t=0，底部）不透明红/绿，第 1 行 alpha 0 */
        const unsigned char img[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 0, 255, 255, 255, 0};
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GLuint pr = prog("#version 300 es\nin vec2 aPos; out vec2 vUV;\nvoid main(){ vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }",
                         "#version 300 es\nprecision mediump float;\nuniform sampler2D t; in vec2 vUV; out vec4 o;\nvoid main(){ vec4 c = texture(t, vUV); if (c.a < 0.5) discard; o = c; }");
        glUseProgram(pr);
        glClear(GL_COLOR_BUFFER_BIT);
        const float pos[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        espgpu_gles_swap();
        px(40, 40, p);
        CHECK(close_to(p, 255, 0, 0)); /* 左下 = 纹理 (0,0) */
        px(280, 40, p);
        CHECK(close_to(p, 0, 255, 0));
        px(40, 160, p);
        CHECK(close_to(p, 0, 0, 0)); /* 上半 alpha 0 被丢弃 */

        /* FBO：渲染到 64x64 纹理，下半红上半绿，再画到屏幕上 */
        GLuint rt, fbo;
        glGenTextures(1, &rt);
        glBindTexture(GL_TEXTURE_2D, rt);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
        CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glViewport(0, 0, 64, 64);
        GLuint pc = prog(kVSPosCol, "#version 300 es\nprecision mediump float;\nin vec4 vCol; out vec4 o;\nvoid main(){ o = vCol; }");
        glUseProgram(pc);
        const float hp[] = {-1, -1, 1, -1, 1, 0, -1, -1, 1, 0, -1, 0, -1, 0, 1, 0, 1, 1, -1, 0, 1, 1, -1, 1};
        float col[48];
        for (int i = 0; i < 12; i++) {
            col[i * 4 + 0] = i < 6 ? 1.0f : 0.0f;
            col[i * 4 + 1] = i < 6 ? 0.0f : 1.0f;
            col[i * 4 + 2] = 0;
            col[i * 4 + 3] = 1;
        }
        const GLint lp = glGetAttribLocation(pc, "aPos"), lc = glGetAttribLocation(pc, "aCol");
        glEnableVertexAttribArray(lp);
        glEnableVertexAttribArray(lc);
        glVertexAttribPointer(lp, 2, GL_FLOAT, GL_FALSE, 0, hp);
        glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 0, col);
        glDrawArrays(GL_TRIANGLES, 0, 12);
        glDisableVertexAttribArray(lc);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 320, 200);
        GLuint pt = prog("#version 300 es\nin vec2 aPos; out vec2 vUV;\nvoid main(){ vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }",
                         "#version 300 es\nprecision mediump float;\nuniform sampler2D t; in vec2 vUV; out vec4 o;\nvoid main(){ o = texture(t, vUV); }");
        glUseProgram(pt);
        glBindTexture(GL_TEXTURE_2D, rt);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        espgpu_gles_swap();
        px(160, 40, p);
        CHECK(close_to(p, 255, 0, 0)); /* FBO 下半（红）出现在屏幕下半 */
        px(160, 160, p);
        CHECK(close_to(p, 0, 255, 0));
    }
    /* 6. VM：uniform 预计算 + SLP（plasma 风格），逐像素对比 C++ */
    {
        GLuint pr = prog("#version 300 es\nin vec2 aPos; out vec2 vUV;\nvoid main(){ vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }",
                         "#version 300 es\nprecision highp float;\nuniform float uTime; in vec2 vUV; out vec4 o;\n"
                         "void main(){ float v = vUV.x * 3.0 + sin(uTime * 0.7) / 2.0; o = vec4(0.5 + 0.5 * sin(v), 0.5 + 0.5 * sin(v + 2.0), clamp(vUV.y * 2.0 - 0.5, 0.0, 1.0), 1.0); }");
        glUseProgram(pr);
        glUniform1f(glGetUniformLocation(pr, "uTime"), 1.7f);
        const float pos[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        espgpu_gles_swap();
        int bad = 0;
        for (int y = 5; y < 200; y += 37)
            for (int x = 3; x < 320; x += 41) {
                px(x, y, p);
                const float u = (x + 0.5f) / 320, w = (y + 0.5f) / 200;
                const float v = u * 3 + std::sin(1.7f * 0.7f) / 2;
                const int r = int((0.5f + 0.5f * std::sin(v)) * 255 + 0.5f), g = int((0.5f + 0.5f * std::sin(v + 2)) * 255 + 0.5f);
                const int b = int(std::fmin(std::fmax(w * 2 - 0.5f, 0.0f), 1.0f) * 255 + 0.5f);
                if (!close_to(p, r, g, b, 14)) bad++;
            }
        CHECK(bad == 0);
    }
    /* 7. 稀疏索引：大 VBO 里只画最后 4 个顶点（只发用到的顶点） */
    {
        GLuint pr = prog(kVSPosCol, "#version 300 es\nprecision mediump float;\nin vec4 vCol; out vec4 o;\nvoid main(){ o = vCol; }");
        glUseProgram(pr);
        std::vector<float> v(5000 * 6, 0.0f);
        const float quad[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int i = 0; i < 4; i++) {
            float *q = &v[(4996 + i) * 6];
            q[0] = quad[i][0];
            q[1] = quad[i][1];
            q[2] = 1; q[3] = 1; q[4] = 0; q[5] = 1;
        }
        GLuint vao, vbo, ibo;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * 4), v.data(), GL_STATIC_DRAW);
        const GLint lp = glGetAttribLocation(pr, "aPos"), lc = glGetAttribLocation(pr, "aCol");
        glEnableVertexAttribArray(lp);
        glEnableVertexAttribArray(lc);
        glVertexAttribPointer(lp, 2, GL_FLOAT, GL_FALSE, 24, nullptr);
        glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 24, reinterpret_cast<void *>(8));
        const unsigned short idx2[] = {4996, 4997, 4998, 4996, 4998, 4999};
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx2), idx2, GL_STATIC_DRAW);
        shim_reset_bytes();
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        std::printf("  draw 7 (indexed into a 5000-vertex VBO): %zu bytes\n", shim_bytes());
        CHECK(shim_bytes() < 400);
        espgpu_gles_swap();
        px(160, 100, p);
        CHECK(close_to(p, 255, 255, 0));
        /* 客户端数组 + 非默认 VAO：ES 3.0 报 GL_INVALID_OPERATION */
        while (glGetError() != GL_NO_ERROR) {}
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(lp, 2, GL_FLOAT, GL_FALSE, 0, v.data());
        CHECK(glGetError() == GL_INVALID_OPERATION);
        glBindVertexArray(0);
    }
    /* 8. PBO 越界：报错而不是越界读 */
    {
        GLuint pbo, tex;
        glGenBuffers(1, &pbo);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, 16, nullptr, GL_STATIC_DRAW);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        while (glGetError() != GL_NO_ERROR) {}
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        CHECK(glGetError() == GL_INVALID_OPERATION);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        CHECK(glGetError() == GL_NO_ERROR);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        GLuint pack;
        glGenBuffers(1, &pack);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
        glBufferData(GL_PIXEL_PACK_BUFFER, 8, nullptr, GL_STATIC_DRAW);
        glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        CHECK(glGetError() == GL_INVALID_OPERATION);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    /* 9. 无表面 MakeCurrent 不能把视口定成 1x1 */
    {
        void *c2 = espgpu_gles_create_context(nullptr, 3);
        espgpu_gles_make_current(c2, nullptr, 0, 0);
        espgpu_gles_make_current(c2, nullptr, 320, 200);
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        CHECK(vp[2] == 320 && vp[3] == 200);
        espgpu_gles_make_current(ctx, nullptr, 320, 200);
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
