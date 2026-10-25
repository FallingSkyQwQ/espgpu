/*
 * window.cpp — GLES demo 的公共部分
 */

#include "window.h"

#include <cstdio>
#include <string>

namespace {

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1; /* 画面由 SwapBuffers 整块覆盖，不擦背景以免闪烁 */
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void fatal(const char *what)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s\n\nEGL error 0x%04X.\n\nIs the ESP32-S3 GPU connected? "
                                    "Set ESPGPU_EMULATOR=1 to use the software device.",
                  what, static_cast<unsigned>(eglGetError()));
    MessageBoxA(nullptr, buf, "espgpu GLES demo", MB_ICONERROR);
}

std::string shader_log(GLuint s)
{
    GLint n = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &n);
    std::string log(static_cast<size_t>(n > 0 ? n : 1), '\0');
    glGetShaderInfoLog(s, n, nullptr, log.data());
    return log;
}

GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    const std::string log = shader_log(s);
    OutputDebugStringA(log.c_str());
    if (!ok) {
        MessageBoxA(nullptr, log.c_str(), type == GL_VERTEX_SHADER ? "vertex shader" : "fragment shader", MB_ICONERROR);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

bool demo_init(DemoWindow &w, const wchar_t *title, int width, int height)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ESPGPU_GLES_DEMO";
    RegisterClassExW(&wc);

    RECT rc{0, 0, width, height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    w.hwnd = CreateWindowW(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!w.hwnd) return false;
    ShowWindow(w.hwnd, SW_SHOW);

    w.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(w.dpy, &major, &minor)) {
        fatal("eglInitialize failed");
        return false;
    }
    const EGLint cfg_attr[] = {EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_DEPTH_SIZE, 16,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(w.dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        fatal("eglChooseConfig found no config");
        return false;
    }
    w.surf = eglCreateWindowSurface(w.dpy, cfg, w.hwnd, nullptr);
    if (w.surf == EGL_NO_SURFACE) {
        fatal("eglCreateWindowSurface failed");
        return false;
    }
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    w.ctx = eglCreateContext(w.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (w.ctx == EGL_NO_CONTEXT || !eglMakeCurrent(w.dpy, w.surf, w.surf, w.ctx)) {
        fatal("eglCreateContext / eglMakeCurrent failed");
        return false;
    }
    eglSwapInterval(w.dpy, 1);
    w.width = width;
    w.height = height;
    return true;
}

bool demo_pump(DemoWindow &w)
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    w.width = rc.right - rc.left;
    w.height = rc.bottom - rc.top;
    return IsWindow(w.hwnd) != 0;
}

void demo_swap(DemoWindow &w)
{
    eglSwapBuffers(w.dpy, w.surf);
}

void demo_shutdown(DemoWindow &w)
{
    if (w.dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(w.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (w.ctx != EGL_NO_CONTEXT) eglDestroyContext(w.dpy, w.ctx);
        if (w.surf != EGL_NO_SURFACE) eglDestroySurface(w.dpy, w.surf);
        eglTerminate(w.dpy);
    }
    if (w.hwnd && IsWindow(w.hwnd)) DestroyWindow(w.hwnd);
    w = DemoWindow{};
}

GLuint demo_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        MessageBoxA(nullptr, log, "link failed", MB_ICONERROR);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

double demo_time()
{
    static LARGE_INTEGER freq{}, t0{};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
}
