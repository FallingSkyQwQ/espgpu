/*
 * gl_wgl.cpp — WGL 入口
 *
 * gdi32 的 ChoosePixelFormat / SetPixelFormat / DescribePixelFormat / SwapBuffers 会转进
 * 已加载的 opengl32.dll 的 wgl* 实现，所以把本 DLL 放在应用目录下即可接管（SDL、DSDA-Doom 都是这样
 * LoadLibrary("opengl32.dll")）。
 *
 * 导出名在 opengl32.def 里映射：wglFoo = ewglFoo（避免与 wingdi.h 的 dllimport 声明冲突）。
 * 后端在第一次 wgl 调用时决定：espgpu 打不开设备就退回直通（Mesa / 系统 GL）。
 */

#include "gl_context.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace espgl {
const char *extension_string();
}

using namespace espgl;

namespace {

std::mutex g_pf_mu;
std::map<HWND, int> g_pf;         /* 每个窗口选定的像素格式（espgpu 模式） */
std::map<HDC, int> g_pf_nowin;    /* 没有窗口的 DC（离屏 / 打印机） */
std::once_flag g_backend_once;

constexpr int kNumFormats = 3;

const char *kWglExt =
    "WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_EXT_swap_control WGL_ARB_pixel_format "
    "WGL_ARB_create_context";

void fill_pfd(int fmt, PIXELFORMATDESCRIPTOR *pfd)
{
    memset(pfd, 0, sizeof(*pfd));
    pfd->nSize = sizeof(*pfd);
    pfd->nVersion = 1;
    pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_GENERIC_ACCELERATED;
    if (fmt != 3) {
        pfd->dwFlags |= PFD_DOUBLEBUFFER;
    }
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->cColorBits = 32;
    pfd->cRedBits = 8;
    pfd->cRedShift = 16;
    pfd->cGreenBits = 8;
    pfd->cGreenShift = 8;
    pfd->cBlueBits = 8;
    pfd->cBlueShift = 0;
    pfd->cAlphaBits = 8;
    pfd->cAlphaShift = 24;
    pfd->cDepthBits = fmt == 2 ? 16 : 24;
    pfd->cStencilBits = fmt == 2 ? 0 : 8;
    pfd->iLayerType = PFD_MAIN_PLANE;
}

/** 首次 wgl 调用：决定后端（打开设备或退回直通） */
void ensure_backend()
{
    std::call_once(g_backend_once, [] {
        mode_init();
        if (g_mode == MODE_ESPGPU && !device_open()) {
            switch_to_passthrough("ESP32-S3 GPU not available");
        }
        if (g_watch) {
            std::atexit([] { trace_dump(); });
        }
    });
}

inline bool pt()
{
    return g_mode == MODE_PASSTHROUGH;
}

Context *ctx_from(HGLRC h)
{
    return reinterpret_cast<Context *>(h);
}

bool is_our_context(HGLRC h)
{
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    for (Context *c : g_sh.contexts) {
        if (reinterpret_cast<HGLRC>(c) == h) {
            return true;
        }
    }
    return false;
}

void update_window_size(Context *c)
{
    if (c->hwnd == nullptr) {
        return;
    }
    RECT rc;
    if (GetClientRect(c->hwnd, &rc)) {
        c->win_w = std::max(1L, rc.right - rc.left);
        c->win_h = std::max(1L, rc.bottom - rc.top);
    }
}

/* ── WGL 扩展（经 wglGetProcAddress） ─────────────────────────────── */

const char *WINAPI x_wglGetExtensionsStringARB(HDC)
{
    return kWglExt;
}

const char *WINAPI x_wglGetExtensionsStringEXT()
{
    return kWglExt;
}

BOOL WINAPI x_wglSwapIntervalEXT(int interval)
{
    if (Context *c = cur()) {
        c->swap_interval = interval;
    }
    return TRUE;
}

int WINAPI x_wglGetSwapIntervalEXT()
{
    Context *c = cur();
    return c ? c->swap_interval : 1;
}

int pf_attrib(int fmt, int attr)
{
    switch (attr) {
    case 0x2000: return kNumFormats;           /* NUMBER_PIXEL_FORMATS */
    case 0x2001: return 1;                     /* DRAW_TO_WINDOW */
    case 0x2002: return 0;                     /* DRAW_TO_BITMAP */
    case 0x2003: return 0x2027;                /* ACCELERATION = FULL */
    case 0x2007: return 0x2028;                /* SWAP_METHOD = EXCHANGE */
    case 0x200F: return 0;                     /* SUPPORT_GDI */
    case 0x2010: return 1;                     /* SUPPORT_OPENGL */
    case 0x2011: return fmt != 3;              /* DOUBLE_BUFFER */
    case 0x2012: return 0;                     /* STEREO */
    case 0x2013: return 0x202B;                /* PIXEL_TYPE = RGBA */
    case 0x2014: return 32;                    /* COLOR_BITS */
    case 0x2015: return 8;                     /* RED_BITS */
    case 0x2016: return 16;
    case 0x2017: return 8;                     /* GREEN_BITS */
    case 0x2018: return 8;
    case 0x2019: return 8;                     /* BLUE_BITS */
    case 0x201A: return 0;
    case 0x201B: return 8;                     /* ALPHA_BITS */
    case 0x201C: return 24;
    case 0x2022: return fmt == 2 ? 16 : 24;    /* DEPTH_BITS */
    case 0x2023: return fmt == 2 ? 0 : 8;      /* STENCIL_BITS */
    default: return 0;
    }
}

BOOL WINAPI x_wglGetPixelFormatAttribivARB(HDC, int fmt, int, UINT n, const int *attribs, int *values)
{
    if (attribs == nullptr || values == nullptr) {
        return FALSE;
    }
    for (UINT i = 0; i < n; i++) {
        values[i] = pf_attrib(fmt, attribs[i]);
    }
    return TRUE;
}

BOOL WINAPI x_wglGetPixelFormatAttribfvARB(HDC, int fmt, int, UINT n, const int *attribs, FLOAT *values)
{
    if (attribs == nullptr || values == nullptr) {
        return FALSE;
    }
    for (UINT i = 0; i < n; i++) {
        values[i] = static_cast<FLOAT>(pf_attrib(fmt, attribs[i]));
    }
    return TRUE;
}

BOOL WINAPI x_wglChoosePixelFormatARB(HDC, const int *iattr, const FLOAT *, UINT max, int *formats, UINT *num)
{
    int want_depth = 0, want_stencil = 0, want_db = 1;
    for (const int *a = iattr; a && a[0]; a += 2) {
        if (a[0] == 0x2022) want_depth = a[1];
        if (a[0] == 0x2023) want_stencil = a[1];
        if (a[0] == 0x2011) want_db = a[1];
        if (a[0] == 0x2042 && a[1] > 1) { /* 要多重采样：没有 */
            if (num) *num = 0;
            return TRUE;
        }
    }
    int fmt = want_db ? 1 : 3;
    if (want_db && want_depth <= 16 && want_stencil == 0) {
        fmt = 1; /* 24/8 仍然满足 */
    }
    if (max > 0 && formats) {
        formats[0] = fmt;
    }
    if (num) {
        *num = 1;
    }
    return TRUE;
}

extern "C" HGLRC WINAPI ewglCreateContext(HDC hdc);

HGLRC WINAPI x_wglCreateContextAttribsARB(HDC hdc, HGLRC share, const int *attribs)
{
    (void)share;
    int major = 1, profile = 0;
    for (const int *a = attribs; a && a[0]; a += 2) {
        if (a[0] == 0x2091) major = a[1];         /* WGL_CONTEXT_MAJOR_VERSION_ARB */
        if (a[0] == 0x9126) profile = a[1];       /* WGL_CONTEXT_PROFILE_MASK_ARB */
    }
    if (major >= 3 && (profile & 1)) {
        SetLastError(0x2095); /* ERROR_INVALID_VERSION_ARB */
        return nullptr;
    }
    if (major >= 3) {
        SetLastError(0x2095);
        return nullptr;
    }
    return ewglCreateContext(hdc);
}

struct WglExt {
    const char *name;
    void *proc;
};

const WglExt kWglProcs[] = {
    {"wglGetExtensionsStringARB", reinterpret_cast<void *>(&x_wglGetExtensionsStringARB)},
    {"wglGetExtensionsStringEXT", reinterpret_cast<void *>(&x_wglGetExtensionsStringEXT)},
    {"wglSwapIntervalEXT", reinterpret_cast<void *>(&x_wglSwapIntervalEXT)},
    {"wglGetSwapIntervalEXT", reinterpret_cast<void *>(&x_wglGetSwapIntervalEXT)},
    {"wglGetPixelFormatAttribivARB", reinterpret_cast<void *>(&x_wglGetPixelFormatAttribivARB)},
    {"wglGetPixelFormatAttribfvARB", reinterpret_cast<void *>(&x_wglGetPixelFormatAttribfvARB)},
    {"wglChoosePixelFormatARB", reinterpret_cast<void *>(&x_wglChoosePixelFormatARB)},
    {"wglCreateContextAttribsARB", reinterpret_cast<void *>(&x_wglCreateContextAttribsARB)},
    {nullptr, nullptr},
};

void *find_proc(const char *name)
{
    for (const ProcEntry *e = kProcTable; e->name; e++) {
        if (strcmp(e->name, name) == 0) {
            return e->proc;
        }
    }
    return nullptr;
}

} // namespace

extern "C" {

int WINAPI ewglChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *pfd)
{
    ensure_backend();
    trace_note("wglChoosePixelFormat(%p)", static_cast<void *>(hdc));
    if (pt()) {
        return g_ptw.ChoosePixelFormat ? g_ptw.ChoosePixelFormat(hdc, pfd) : 0;
    }
    if (pfd && !(pfd->dwFlags & PFD_DOUBLEBUFFER) && !(pfd->dwFlags & PFD_DOUBLEBUFFER_DONTCARE)) {
        return 3;
    }
    if (pfd && pfd->cDepthBits > 0 && pfd->cDepthBits <= 16 && pfd->cStencilBits == 0) {
        return 2;
    }
    return 1;
}

int WINAPI ewglDescribePixelFormat(HDC hdc, int fmt, UINT bytes, LPPIXELFORMATDESCRIPTOR pfd)
{
    ensure_backend();
    if (pt()) {
        return g_ptw.DescribePixelFormat ? g_ptw.DescribePixelFormat(hdc, fmt, bytes, pfd) : 0;
    }
    if (pfd && bytes >= sizeof(PIXELFORMATDESCRIPTOR) && fmt >= 1 && fmt <= kNumFormats) {
        fill_pfd(fmt, pfd);
    }
    return kNumFormats;
}

int WINAPI ewglGetPixelFormat(HDC hdc)
{
    ensure_backend();
    if (pt()) {
        return g_ptw.GetPixelFormat ? g_ptw.GetPixelFormat(hdc) : 0;
    }
    std::lock_guard<std::mutex> lk(g_pf_mu);
    HWND w = WindowFromDC(hdc);
    if (w) {
        auto it = g_pf.find(w);
        return it == g_pf.end() ? 0 : it->second;
    }
    auto it = g_pf_nowin.find(hdc);
    return it == g_pf_nowin.end() ? 0 : it->second;
}

BOOL WINAPI ewglSetPixelFormat(HDC hdc, int fmt, const PIXELFORMATDESCRIPTOR *pfd)
{
    ensure_backend();
    trace_note("wglSetPixelFormat(%p, %d)", static_cast<void *>(hdc), fmt);
    if (pt()) {
        return g_ptw.SetPixelFormat ? g_ptw.SetPixelFormat(hdc, fmt, pfd) : FALSE;
    }
    if (fmt < 1 || fmt > kNumFormats) {
        SetLastError(ERROR_INVALID_PIXEL_FORMAT);
        return FALSE;
    }
    std::lock_guard<std::mutex> lk(g_pf_mu);
    HWND w = WindowFromDC(hdc);
    if (w) {
        g_pf[w] = fmt;
    } else {
        g_pf_nowin[hdc] = fmt;
    }
    return TRUE;
}

HGLRC WINAPI ewglCreateContext(HDC hdc)
{
    ensure_backend();
    trace_note("wglCreateContext(%p)", static_cast<void *>(hdc));
    if (pt()) {
        return g_ptw.CreateContext ? g_ptw.CreateContext(hdc) : nullptr;
    }
    auto *c = new Context();
    c->hdc = hdc;
    c->hwnd = WindowFromDC(hdc);
    update_window_size(c);
    std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
    g_sh.contexts.push_back(c);
    return reinterpret_cast<HGLRC>(c);
}

HGLRC WINAPI ewglCreateLayerContext(HDC hdc, int layer)
{
    ensure_backend();
    if (pt()) {
        return g_ptw.CreateLayerContext ? g_ptw.CreateLayerContext(hdc, layer) : nullptr;
    }
    return layer == 0 ? ewglCreateContext(hdc) : nullptr;
}

BOOL WINAPI ewglDeleteContext(HGLRC h)
{
    ensure_backend();
    if (pt()) {
        return g_ptw.DeleteContext ? g_ptw.DeleteContext(h) : FALSE;
    }
    if (!is_our_context(h)) {
        return FALSE;
    }
    Context *c = ctx_from(h);
    if (cur() == c) {
        flush_pending(c);
        set_cur(nullptr);
    }
    if (g_sh.dc_owner == c) {
        g_sh.dc_owner = nullptr;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
        auto &v = g_sh.contexts;
        v.erase(std::remove(v.begin(), v.end(), c), v.end());
    }
    delete c;
    return TRUE;
}

BOOL WINAPI ewglMakeCurrent(HDC hdc, HGLRC h)
{
    ensure_backend();
    if (pt()) {
        return g_ptw.MakeCurrent ? g_ptw.MakeCurrent(hdc, h) : FALSE;
    }
    Context *old = cur();
    if (h == nullptr) {
        if (old) {
            flush_pending(old);
        }
        set_cur(nullptr);
        return TRUE;
    }
    if (!is_our_context(h)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    Context *c = ctx_from(h);
    if (old && old != c) {
        flush_pending(old);
    }
    if (hdc) {
        c->hdc = hdc;
        HWND w = WindowFromDC(hdc);
        if (w) {
            c->hwnd = w;
        }
    }
    update_window_size(c);
    if (!c->viewport_init) {
        c->st.viewport[0] = c->st.viewport[1] = 0;
        c->st.viewport[2] = c->win_w;
        c->st.viewport[3] = c->win_h;
        c->st.scissor_box[0] = c->st.scissor_box[1] = 0;
        c->st.scissor_box[2] = c->win_w;
        c->st.scissor_box[3] = c->win_h;
        c->viewport_init = true;
    }
    set_cur(c);
    return TRUE;
}

HGLRC WINAPI ewglGetCurrentContext(void)
{
    if (pt()) {
        return g_ptw.GetCurrentContext ? g_ptw.GetCurrentContext() : nullptr;
    }
    return reinterpret_cast<HGLRC>(cur());
}

HDC WINAPI ewglGetCurrentDC(void)
{
    if (pt()) {
        return g_ptw.GetCurrentDC ? g_ptw.GetCurrentDC() : nullptr;
    }
    Context *c = cur();
    return c ? c->hdc : nullptr;
}

PROC WINAPI ewglGetProcAddress(LPCSTR name)
{
    ensure_backend();
    if (name == nullptr) {
        return nullptr;
    }
    void *p = find_proc(name);
    if (p == nullptr) {
        for (const WglExt *e = kWglProcs; e->name; e++) {
            if (strcmp(e->name, name) == 0) {
                p = e->proc;
                break;
            }
        }
        if (p && pt()) {
            p = nullptr; /* 直通模式下 WGL 扩展用真实实现 */
        }
    }
    if (p && pt()) {
        /* 直通：真实 GL 没有的入口不要冒充提供（包装会变成空操作） */
        void *real = g_ptw.GetProcAddress ? reinterpret_cast<void *>(g_ptw.GetProcAddress(name)) : nullptr;
        if (real == nullptr) {
            real = pt_sym(g_ptw.module, name);
        }
        if (real == nullptr) {
            p = nullptr;
        }
        trace_proc_request(name, p != nullptr);
        return reinterpret_cast<PROC>(p);
    }
    if (p == nullptr && pt() && g_ptw.GetProcAddress) {
        PROC r = g_ptw.GetProcAddress(name);
        trace_proc_request(name, r != nullptr);
        return r;
    }
    trace_proc_request(name, p != nullptr);
    return reinterpret_cast<PROC>(p);
}

PROC WINAPI ewglGetDefaultProcAddress(LPCSTR name)
{
    return ewglGetProcAddress(name);
}

BOOL WINAPI ewglShareLists(HGLRC a, HGLRC b)
{
    ensure_backend();
    if (pt()) {
        return g_ptw.ShareLists ? g_ptw.ShareLists(a, b) : FALSE;
    }
    return TRUE; /* 所有上下文本来就共用一个对象命名空间 */
}

BOOL WINAPI ewglCopyContext(HGLRC src, HGLRC dst, UINT mask)
{
    ensure_backend();
    if (pt()) {
        return g_ptw.CopyContext ? g_ptw.CopyContext(src, dst, mask) : FALSE;
    }
    if (!is_our_context(src) || !is_our_context(dst)) {
        return FALSE;
    }
    ctx_from(dst)->st = ctx_from(src)->st;
    return TRUE;
}

BOOL WINAPI ewglSwapBuffers(HDC hdc)
{
    ensure_backend();
    if (g_watch && g_trace_log) {
        trace_note("wglSwapBuffers(%p)", static_cast<void *>(hdc));
    }
    if (pt()) {
        const BOOL r = g_ptw.SwapBuffers ? g_ptw.SwapBuffers(hdc) : FALSE;
        trace_frame();
        return r;
    }
    Context *c = cur();
    if (c == nullptr || (hdc && c->hdc != hdc && WindowFromDC(hdc) != c->hwnd)) {
        /* 不是当前上下文的 DC：找绑定在这个窗口上的上下文 */
        std::lock_guard<std::recursive_mutex> lk(g_sh.mu);
        HWND w = WindowFromDC(hdc);
        for (Context *x : g_sh.contexts) {
            if (x->hdc == hdc || (w && x->hwnd == w)) {
                c = x;
                break;
            }
        }
    }
    if (c) {
        device_present(c);
    }
    trace_frame();
    return TRUE;
}

BOOL WINAPI ewglSwapLayerBuffers(HDC hdc, UINT planes)
{
    if (pt() && g_ptw.SwapLayerBuffers) {
        const BOOL r = g_ptw.SwapLayerBuffers(hdc, planes);
        trace_frame();
        return r;
    }
    return ewglSwapBuffers(hdc);
}

DWORD WINAPI ewglSwapMultipleBuffers(UINT n, const WGLSWAP *swaps)
{
    for (UINT i = 0; i < n && swaps; i++) {
        ewglSwapBuffers(swaps[i].hdc);
    }
    return 0;
}

static BOOL use_font_bitmaps(HDC hdc, DWORD first, DWORD count, DWORD base, bool wide)
{
    ensure_backend();
    if (pt()) {
        auto fn = wide ? g_ptw.UseFontBitmapsW : g_ptw.UseFontBitmapsA;
        return fn ? fn(hdc, first, count, base) : FALSE;
    }
    (void)first;
    /* 位图字体不支持：建空列表，glCallLists 什么也不画，但程序不会因此失败 */
    for (DWORD i = 0; i < count; i++) {
        impl_glNewList(base + i, GL_COMPILE);
        impl_glEndList();
    }
    return TRUE;
}

BOOL WINAPI ewglUseFontBitmapsA(HDC hdc, DWORD first, DWORD count, DWORD base)
{
    return use_font_bitmaps(hdc, first, count, base, false);
}

BOOL WINAPI ewglUseFontBitmapsW(HDC hdc, DWORD first, DWORD count, DWORD base)
{
    return use_font_bitmaps(hdc, first, count, base, true);
}

BOOL WINAPI ewglUseFontOutlinesA(HDC, DWORD, DWORD, DWORD, FLOAT, FLOAT, int, LPGLYPHMETRICSFLOAT)
{
    return FALSE;
}

BOOL WINAPI ewglUseFontOutlinesW(HDC, DWORD, DWORD, DWORD, FLOAT, FLOAT, int, LPGLYPHMETRICSFLOAT)
{
    return FALSE;
}

BOOL WINAPI ewglDescribeLayerPlane(HDC, int, int, UINT, LPLAYERPLANEDESCRIPTOR)
{
    return FALSE;
}

int WINAPI ewglSetLayerPaletteEntries(HDC, int, int, int, const COLORREF *)
{
    return 0;
}

int WINAPI ewglGetLayerPaletteEntries(HDC, int, int, int, COLORREF *)
{
    return 0;
}

BOOL WINAPI ewglRealizeLayerPalette(HDC, int, BOOL)
{
    return FALSE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        /* FreeLibrary 卸载（不是进程退出）：把清单落盘；进程退出由 atexit 负责 */
        trace_dump();
    }
    return TRUE;
}

} // extern "C"
