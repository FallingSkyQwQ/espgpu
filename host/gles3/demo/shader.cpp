/*
 * gles-shader — M6 验收：移植一个需要片元 VM 的着色器
 *
 * 背景是经典的 plasma（多组 sin 叠加 + 调色），由 GLSL 编译成 VM 字节码在设备上
 * 逐像素执行；前景是一张程序生成的棋盘纹理，顶点着色器里用 sin/cos 做旋转
 * 和摆动（不是纯矩阵变换，主机端解释执行），片元着色器加了暗角和 alpha 裁剪。
 */

#include "window.h"

#include <cmath>
#include <vector>

static const char *kPlasmaVS = R"(#version 300 es
in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char *kPlasmaFS = R"(#version 300 es
precision mediump float;
uniform float uTime;
uniform vec2 uRes;
in vec2 vUV;
out vec4 fragColor;
void main() {
    vec2 p = vUV * 2.0 - 1.0;
    p.x *= uRes.x / uRes.y;
    float v = sin(p.x * 3.0 + uTime);
    v += sin((p.y * 3.0 + uTime) * 0.5);
    v += sin((p.x * 2.0 + p.y * 2.0 + uTime) * 0.7);
    vec2 c = p + vec2(sin(uTime * 0.3), cos(uTime * 0.5)) * 0.8;
    v += sin(sqrt(dot(c, c) * 8.0 + 1.0) + uTime);
    v *= 0.5;
    vec3 col = vec3(0.5 + 0.5 * sin(3.14159 * v),
                    0.5 + 0.5 * sin(3.14159 * v + 2.094),
                    0.5 + 0.5 * sin(3.14159 * v + 4.188));
    fragColor = vec4(col, 1.0);
}
)";

static const char *kCardVS = R"(#version 300 es
in vec2 aPos;
in vec2 aUV;
uniform float uTime;
uniform float uAspect;
out vec2 vUV;
void main() {
    float a = uTime * 0.7;
    mat2 r = mat2(cos(a), sin(a), -sin(a), cos(a));
    vec2 p = r * (aPos * 0.45);
    p.y += 0.08 * sin(uTime * 2.0 + aPos.x * 3.0);
    p.x /= uAspect;
    vUV = aUV;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

static const char *kCardFS = R"(#version 300 es
precision mediump float;
uniform sampler2D uTex;
uniform vec4 uTint;
in vec2 vUV;
out vec4 fragColor;
void main() {
    vec2 d = vUV - 0.5;
    float r = length(d);
    if (r > 0.5) discard;                 /* 圆形卡片 */
    vec4 c = texture(uTex, vUV) * uTint;
    float vign = smoothstep(0.5, 0.25, r);
    fragColor = vec4(c.rgb * (0.4 + 0.6 * vign), 1.0);
}
)";

static GLuint make_checker()
{
    const int n = 64;
    std::vector<unsigned char> px(n * n * 4);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            const bool on = ((x / 8) + (y / 8)) & 1;
            unsigned char *p = &px[(y * n + x) * 4];
            p[0] = on ? 240 : 30;
            p[1] = on ? 220 : 40;
            p[2] = on ? 180 : 70;
            p[3] = 255;
        }
    }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return t;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    DemoWindow w;
    if (!demo_init(w, L"espgpu GLES 3.0 - plasma (fragment VM)", 640, 400)) return 1;

    GLuint plasma = demo_program(kPlasmaVS, kPlasmaFS);
    GLuint card = demo_program(kCardVS, kCardFS);
    if (!plasma || !card) return 1;

    /* 全屏四边形：两个三角形 */
    const float quad[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
    GLuint vao_bg, vbo_bg;
    glGenVertexArrays(1, &vao_bg);
    glBindVertexArray(vao_bg);
    glGenBuffers(1, &vbo_bg);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_bg);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    const GLint bg_pos = glGetAttribLocation(plasma, "aPos");
    glVertexAttribPointer(static_cast<GLuint>(bg_pos), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(static_cast<GLuint>(bg_pos));

    /* 卡片：带 UV 的四边形，索引绘制 */
    const float card_v[] = {-1, -1, 0, 0, 1, -1, 2, 0, 1, 1, 2, 2, -1, 1, 0, 2};
    const unsigned short card_i[] = {0, 1, 2, 0, 2, 3};
    GLuint vao_card, vbo_card, ibo_card;
    glGenVertexArrays(1, &vao_card);
    glBindVertexArray(vao_card);
    glGenBuffers(1, &vbo_card);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_card);
    glBufferData(GL_ARRAY_BUFFER, sizeof(card_v), card_v, GL_STATIC_DRAW);
    glGenBuffers(1, &ibo_card);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_card);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(card_i), card_i, GL_STATIC_DRAW);
    const GLint c_pos = glGetAttribLocation(card, "aPos");
    const GLint c_uv = glGetAttribLocation(card, "aUV");
    glVertexAttribPointer(static_cast<GLuint>(c_pos), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribPointer(static_cast<GLuint>(c_uv), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void *>(2 * sizeof(float)));
    glEnableVertexAttribArray(static_cast<GLuint>(c_pos));
    glEnableVertexAttribArray(static_cast<GLuint>(c_uv));

    GLuint tex = make_checker();

    const GLint u_time = glGetUniformLocation(plasma, "uTime");
    const GLint u_res = glGetUniformLocation(plasma, "uRes");
    const GLint uc_time = glGetUniformLocation(card, "uTime");
    const GLint uc_aspect = glGetUniformLocation(card, "uAspect");
    const GLint uc_tex = glGetUniformLocation(card, "uTex");
    const GLint uc_tint = glGetUniformLocation(card, "uTint");

    while (demo_pump(w)) {
        const float t = static_cast<float>(demo_time());
        const float aspect = w.height > 0 ? static_cast<float>(w.width) / static_cast<float>(w.height) : 1.0f;
        glViewport(0, 0, w.width, w.height);

        glUseProgram(plasma);
        glUniform1f(u_time, t);
        glUniform2f(u_res, static_cast<float>(w.width), static_cast<float>(w.height));
        glBindVertexArray(vao_bg);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glUseProgram(card);
        glUniform1f(uc_time, t);
        glUniform1f(uc_aspect, aspect);
        glUniform1i(uc_tex, 0);
        glUniform4f(uc_tint, 1.0f, 0.9f + 0.1f * std::sin(t * 3.0f), 0.8f, 1.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindVertexArray(vao_card);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);

        demo_swap(w);
    }

    demo_shutdown(w);
    return 0;
}
