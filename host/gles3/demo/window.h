/*
 * window.h — GLES demo 的公共部分：Win32 窗口 + EGL 初始化 + 着色器编译
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

struct DemoWindow {
    HWND hwnd = nullptr;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    int width = 0;
    int height = 0;
};

/** 建窗口、初始化 EGL、创建 ES3 上下文并设为当前。失败会弹窗说明原因。 */
bool demo_init(DemoWindow &w, const wchar_t *title, int width, int height);

/** 处理消息；收到退出返回 false。同时更新 w.width / w.height。 */
bool demo_pump(DemoWindow &w);

void demo_swap(DemoWindow &w);
void demo_shutdown(DemoWindow &w);

/** 编译 + 链接；出错弹窗显示日志并返回 0。 */
GLuint demo_program(const char *vs, const char *fs);

/** 秒，单调时钟 */
double demo_time();
