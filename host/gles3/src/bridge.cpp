/*
 * bridge.cpp — libGLESv2 给 libEGL 用的内部接口
 *
 * EGL 管窗口表面与上下文生命周期；GL 状态和设备连接都在 libGLESv2 里。
 */

#include "context.h"

#include <algorithm>

using namespace gles;

namespace {

HMODULE self_module()
{
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&self_module), &m);
    return m;
}

} // namespace

ESPGPU_GLES_INTERNAL int espgpu_gles_initialize(void)
{
    return device_open() ? 1 : 0;
}

ESPGPU_GLES_INTERNAL void espgpu_gles_terminate(void)
{
    device_close();
}

ESPGPU_GLES_INTERNAL void espgpu_gles_device_size(int *w, int *h)
{
    *w = device().width;
    *h = device().height;
}

ESPGPU_GLES_INTERNAL void *espgpu_gles_create_context(void *share, int version)
{
    auto *c = new Context();
    c->version = version >= 3 ? 3 : 2;
    c->sh = share ? static_cast<Context *>(share)->sh : std::make_shared<Shared>();
    return c;
}

ESPGPU_GLES_INTERNAL void espgpu_gles_destroy_context(void *ctx)
{
    auto *c = static_cast<Context *>(ctx);
    if (!c) return;
    if (current() == c) set_current(nullptr);
    delete c;
}

/** hwnd 为 NULL 表示 pbuffer / 无表面；w/h 是表面尺寸 */
ESPGPU_GLES_INTERNAL int espgpu_gles_make_current(void *ctx, void *hwnd, int w, int h)
{
    auto *c = static_cast<Context *>(ctx);
    set_current(c);
    if (!c) return 1;
    c->hwnd = static_cast<HWND>(hwnd);
    if (c->hwnd) {
        RECT rc;
        if (GetClientRect(c->hwnd, &rc)) {
            w = rc.right - rc.left;
            h = rc.bottom - rc.top;
        }
    }
    const bool has_surface = c->hwnd != nullptr || (w > 0 && h > 0);
    if (!has_surface) {
        /* 无表面：按设备尺寸当默认帧缓冲，视口留到第一次绑定真正的表面时再初始化 */
        w = device().width;
        h = device().height;
    }
    c->win_w = std::max(1, w);
    c->win_h = std::max(1, h);
    if (!c->viewport_set && has_surface) {
        /* GL：第一次绑定到表面时视口 / 裁剪框初始化为表面大小 */
        c->viewport[0] = c->viewport[1] = 0;
        c->viewport[2] = c->win_w;
        c->viewport[3] = c->win_h;
        c->scissor[0] = c->scissor[1] = 0;
        c->scissor[2] = c->win_w;
        c->scissor[3] = c->win_h;
        c->viewport_set = true;
    }
    static Context *s_last = nullptr;
    if (s_last != c) device().invalidate(); /* 切换到别的上下文：设备镜像不可信 */
    s_last = c;
    return 1;
}

ESPGPU_GLES_INTERNAL void espgpu_gles_swap(void)
{
    swap_buffers();
}

ESPGPU_GLES_INTERNAL void espgpu_gles_set_swap_interval(int interval)
{
    if (Context *c = current()) c->swap_interval = interval;
}

ESPGPU_GLES_INTERNAL void *espgpu_gles_get_proc(const char *name)
{
    static HMODULE m = self_module();
    if (!m || !name) return nullptr;
    /* 带 OES / EXT 后缀的同名函数（glGenVertexArraysOES 等）映射到核心版本 */
    FARPROC f = GetProcAddress(m, name);
    if (!f) {
        std::string n = name;
        for (const char *suf : {"OES", "EXT", "KHR"}) {
            const size_t L = std::char_traits<char>::length(suf);
            if (n.size() > L && n.compare(n.size() - L, L, suf) == 0) {
                f = GetProcAddress(m, n.substr(0, n.size() - L).c_str());
                if (f) break;
            }
        }
    }
    return reinterpret_cast<void *>(f);
}
