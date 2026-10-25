/*
 * gles-triangle — M6 验收：标准 GLES 3.0 三角形
 *
 * 顶点着色器是 `gl_Position = uRot * vec4(aPos, 0.0, 1.0)`，片元着色器是
 * `fragColor = vColor`：两者都会被编译器识别，变换走设备 T&L、着色走内置
 * FLAT（高洛德插值）模式，完全不经过片元 VM。
 */

#include "window.h"

#include <cmath>

static const char *kVS = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uRot;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uRot * vec4(aPos, 0.0, 1.0);
}
)";

static const char *kFS = R"(#version 300 es
precision mediump float;
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    DemoWindow w;
    if (!demo_init(w, L"espgpu GLES 3.0 - triangle", 640, 400)) return 1;

    GLuint prog = demo_program(kVS, kFS);
    if (!prog) return 1;

    const float verts[] = {
        /* x, y,       r, g, b, a */
        0.0f, 0.8f,    1.0f, 0.2f, 0.2f, 1.0f,
        -0.7f, -0.6f,  0.2f, 1.0f, 0.2f, 1.0f,
        0.7f, -0.6f,   0.2f, 0.4f, 1.0f, 1.0f,
    };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void *>(0));
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void *>(2 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    const GLint loc_rot = glGetUniformLocation(prog, "uRot");

    while (demo_pump(w)) {
        const float t = static_cast<float>(demo_time());
        const float c = std::cos(t), s = std::sin(t);
        /* 宽高比校正：设备 320x200（8:5），窗口任意 */
        const float aspect = w.height > 0 ? static_cast<float>(w.width) / static_cast<float>(w.height) : 1.0f;
        const float m[16] = {
            c / aspect, s, 0, 0,
            -s / aspect, c, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,
        };
        glViewport(0, 0, w.width, w.height);
        glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glUniformMatrix4fv(loc_rot, 1, GL_FALSE, m);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        demo_swap(w);
    }

    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    demo_shutdown(w);
    return 0;
}
