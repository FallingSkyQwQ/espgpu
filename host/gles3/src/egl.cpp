/*
 * egl.cpp — libEGL：EGL 1.4 最小实现（Win32 HWND）
 *
 * 只有一个显示（设备），两个配置：
 *   1. RGB565 + 16 位深度（设备的真实格式）
 *   2. RGBA8888 + 24/8 深度模板（给只认 8 位配置的程序；实际仍按 RGB565 渲染）
 */

#define EGLAPI
#include <EGL/egl.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>

extern "C" {
__declspec(dllimport) int espgpu_gles_initialize(void);
__declspec(dllimport) void espgpu_gles_terminate(void);
__declspec(dllimport) void espgpu_gles_device_size(int *w, int *h);
__declspec(dllimport) void *espgpu_gles_create_context(void *share, int version);
__declspec(dllimport) void espgpu_gles_destroy_context(void *ctx);
__declspec(dllimport) int espgpu_gles_make_current(void *ctx, void *hwnd, int w, int h);
__declspec(dllimport) void espgpu_gles_swap(void);
__declspec(dllimport) void espgpu_gles_set_swap_interval(int interval);
__declspec(dllimport) void *espgpu_gles_get_proc(const char *name);
}

namespace {

struct Config {
    EGLint id, r, g, b, a, depth, stencil;
};
const Config kConfigs[] = {
    {1, 5, 6, 5, 0, 16, 0},
    {2, 8, 8, 8, 8, 24, 8},
};

struct Surface {
    HWND hwnd = nullptr;
    int w = 0, h = 0;
    bool pbuffer = false;
    const Config *cfg = nullptr;
};

struct Ctx {
    void *gl = nullptr;
    int version = 2;
    const Config *cfg = nullptr;
};

EGLDisplay const kDisplay = reinterpret_cast<EGLDisplay>(static_cast<uintptr_t>(1));
bool g_initialized = false;
std::set<Surface *> g_surfaces;
std::set<Ctx *> g_contexts;

thread_local EGLint t_error = EGL_SUCCESS;
thread_local Ctx *t_ctx = nullptr;
thread_local Surface *t_draw = nullptr;
thread_local Surface *t_read = nullptr;
thread_local EGLenum t_api = EGL_OPENGL_ES_API;

template <typename T>
T fail(EGLint e, T v)
{
    t_error = e;
    return v;
}

bool check_display(EGLDisplay d, bool need_init = true)
{
    if (d != kDisplay) return fail(EGL_BAD_DISPLAY, false);
    if (need_init && !g_initialized) return fail(EGL_NOT_INITIALIZED, false);
    return true;
}

const Config *config_of(EGLConfig c)
{
    for (const auto &k : kConfigs) {
        if (reinterpret_cast<EGLConfig>(static_cast<uintptr_t>(k.id)) == c) return &k;
    }
    return nullptr;
}

EGLConfig handle_of(const Config &k)
{
    return reinterpret_cast<EGLConfig>(static_cast<uintptr_t>(k.id));
}

bool config_attrib(const Config &k, EGLint attr, EGLint &v)
{
    int dw = 320, dh = 200;
    if (g_initialized) espgpu_gles_device_size(&dw, &dh);
    switch (attr) {
    case EGL_BUFFER_SIZE: v = k.r + k.g + k.b + k.a; return true;
    case EGL_RED_SIZE: v = k.r; return true;
    case EGL_GREEN_SIZE: v = k.g; return true;
    case EGL_BLUE_SIZE: v = k.b; return true;
    case EGL_ALPHA_SIZE: v = k.a; return true;
    case EGL_DEPTH_SIZE: v = k.depth; return true;
    case EGL_STENCIL_SIZE: v = k.stencil; return true;
    case EGL_CONFIG_ID: v = k.id; return true;
    case EGL_CONFIG_CAVEAT: v = k.id == 1 ? EGL_NONE : EGL_SLOW_CONFIG; return true;
    case EGL_LEVEL: v = 0; return true;
    case EGL_MAX_PBUFFER_WIDTH: v = 4096; return true;
    case EGL_MAX_PBUFFER_HEIGHT: v = 4096; return true;
    case EGL_MAX_PBUFFER_PIXELS: v = 4096 * 4096; return true;
    case EGL_NATIVE_RENDERABLE: v = EGL_FALSE; return true;
    case EGL_NATIVE_VISUAL_ID: v = 0; return true;
    case EGL_NATIVE_VISUAL_TYPE: v = EGL_NONE; return true;
    case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: v = 0; return true;
    case EGL_SURFACE_TYPE: v = EGL_WINDOW_BIT | EGL_PBUFFER_BIT | EGL_SWAP_BEHAVIOR_PRESERVED_BIT; return true;
    case EGL_TRANSPARENT_TYPE: v = EGL_NONE; return true;
    case EGL_TRANSPARENT_RED_VALUE: case EGL_TRANSPARENT_GREEN_VALUE: case EGL_TRANSPARENT_BLUE_VALUE: v = 0; return true;
    case EGL_BIND_TO_TEXTURE_RGB: case EGL_BIND_TO_TEXTURE_RGBA: v = EGL_FALSE; return true;
    case EGL_MIN_SWAP_INTERVAL: v = 0; return true;
    case EGL_MAX_SWAP_INTERVAL: v = 4; return true;
    case EGL_LUMINANCE_SIZE: case EGL_ALPHA_MASK_SIZE: v = 0; return true;
    case EGL_COLOR_BUFFER_TYPE: v = EGL_RGB_BUFFER; return true;
    case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT:
        v = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT;
        return true;
    default: return false;
    }
}

bool matches(const Config &k, const EGLint *attribs)
{
    if (!attribs) return true;
    for (const EGLint *a = attribs; a[0] != EGL_NONE; a += 2) {
        const EGLint attr = a[0], want = a[1];
        if (want == EGL_DONT_CARE) continue;
        EGLint have = 0;
        switch (attr) {
        case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: case EGL_DEPTH_SIZE:
        case EGL_STENCIL_SIZE: case EGL_BUFFER_SIZE:
            config_attrib(k, attr, have);
            if (have < want) return false;
            break;
        case EGL_CONFIG_ID:
            if (k.id != want) return false;
            break;
        case EGL_SURFACE_TYPE: case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT:
            config_attrib(k, attr, have);
            if ((have & want) != want) return false;
            break;
        case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS:
            /* 很多程序默认请求 4x MSAA：设备没有，但宽松接受 */
            break;
        case EGL_COLOR_BUFFER_TYPE:
            if (want != EGL_RGB_BUFFER) return false;
            break;
        case EGL_TRANSPARENT_TYPE:
            if (want != EGL_NONE) return false;
            break;
        case EGL_LEVEL:
            if (want != 0) return false;
            break;
        default: break; /* 未知 / 无关属性：忽略 */
        }
    }
    return true;
}

} // namespace

extern "C" {

EGLAPI EGLint EGLAPIENTRY eglGetError(void)
{
    const EGLint e = t_error;
    t_error = EGL_SUCCESS;
    return e;
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType)
{
    return kDisplay;
}

EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    if (!check_display(dpy, false)) return EGL_FALSE;
    if (!g_initialized) {
        if (!espgpu_gles_initialize()) return fail(EGL_NOT_INITIALIZED, EGL_FALSE);
        g_initialized = true;
    }
    if (major) *major = 1;
    if (minor) *minor = 4;
    t_error = EGL_SUCCESS;
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay dpy)
{
    if (!check_display(dpy, false)) return EGL_FALSE;
    if (!g_initialized) return EGL_TRUE;
    for (Ctx *c : g_contexts) {
        espgpu_gles_destroy_context(c->gl);
        delete c;
    }
    g_contexts.clear();
    for (Surface *s : g_surfaces) delete s;
    g_surfaces.clear();
    t_ctx = nullptr;
    t_draw = t_read = nullptr;
    espgpu_gles_terminate();
    g_initialized = false;
    return EGL_TRUE;
}

EGLAPI const char *EGLAPIENTRY eglQueryString(EGLDisplay dpy, EGLint name)
{
    if (dpy == EGL_NO_DISPLAY && name == EGL_EXTENSIONS) return "EGL_EXT_client_extensions";
    if (!check_display(dpy)) return nullptr;
    switch (name) {
    case EGL_VENDOR: return "espgpu";
    case EGL_VERSION: return "1.4 espgpu";
    case EGL_EXTENSIONS: return "EGL_KHR_create_context EGL_KHR_surfaceless_context";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    default: return fail<const char *>(EGL_BAD_PARAMETER, nullptr);
    }
}

EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config)
{
    if (!check_display(dpy)) return EGL_FALSE;
    if (!num_config) return fail(EGL_BAD_PARAMETER, EGL_FALSE);
    const EGLint n = static_cast<EGLint>(sizeof(kConfigs) / sizeof(kConfigs[0]));
    if (!configs) {
        *num_config = n;
        return EGL_TRUE;
    }
    EGLint k = 0;
    for (; k < n && k < config_size; k++) configs[k] = handle_of(kConfigs[k]);
    *num_config = k;
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                                             EGLint config_size, EGLint *num_config)
{
    if (!check_display(dpy)) return EGL_FALSE;
    if (!num_config) return fail(EGL_BAD_PARAMETER, EGL_FALSE);
    EGLint k = 0;
    for (const auto &c : kConfigs) {
        if (!matches(c, attrib_list)) continue;
        if (configs) {
            if (k >= config_size) break;
            configs[k] = handle_of(c);
        }
        k++;
    }
    *num_config = k;
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)
{
    if (!check_display(dpy)) return EGL_FALSE;
    const Config *c = config_of(config);
    if (!c) return fail(EGL_BAD_CONFIG, EGL_FALSE);
    EGLint v;
    if (!config_attrib(*c, attribute, v)) return fail(EGL_BAD_ATTRIBUTE, EGL_FALSE);
    *value = v;
    return EGL_TRUE;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                                    const EGLint *)
{
    if (!check_display(dpy)) return EGL_NO_SURFACE;
    const Config *c = config_of(config);
    if (!c) return fail(EGL_BAD_CONFIG, EGL_NO_SURFACE);
    if (!win || !IsWindow(win)) return fail(EGL_BAD_NATIVE_WINDOW, EGL_NO_SURFACE);
    auto *s = new Surface();
    s->hwnd = win;
    s->cfg = c;
    RECT rc;
    GetClientRect(win, &rc);
    s->w = rc.right - rc.left;
    s->h = rc.bottom - rc.top;
    g_surfaces.insert(s);
    return s;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list)
{
    if (!check_display(dpy)) return EGL_NO_SURFACE;
    const Config *c = config_of(config);
    if (!c) return fail(EGL_BAD_CONFIG, EGL_NO_SURFACE);
    auto *s = new Surface();
    s->pbuffer = true;
    s->cfg = c;
    s->w = s->h = 1;
    for (const EGLint *a = attrib_list; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_WIDTH) s->w = a[1];
        if (a[0] == EGL_HEIGHT) s->h = a[1];
    }
    g_surfaces.insert(s);
    return s;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePixmapSurface(EGLDisplay, EGLConfig, EGLNativePixmapType, const EGLint *)
{
    return fail(EGL_BAD_MATCH, EGL_NO_SURFACE);
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferFromClientBuffer(EGLDisplay, EGLenum, EGLClientBuffer, EGLConfig,
                                                              const EGLint *)
{
    return fail(EGL_BAD_PARAMETER, EGL_NO_SURFACE);
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    if (!check_display(dpy)) return EGL_FALSE;
    auto *s = static_cast<Surface *>(surface);
    if (!g_surfaces.count(s)) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    g_surfaces.erase(s);
    if (t_draw == s) t_draw = nullptr;
    if (t_read == s) t_read = nullptr;
    delete s;
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value)
{
    if (!check_display(dpy)) return EGL_FALSE;
    auto *s = static_cast<Surface *>(surface);
    if (!g_surfaces.count(s)) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    if (s->hwnd) {
        RECT rc;
        GetClientRect(s->hwnd, &rc);
        s->w = rc.right - rc.left;
        s->h = rc.bottom - rc.top;
    }
    switch (attribute) {
    case EGL_WIDTH: *value = s->w; break;
    case EGL_HEIGHT: *value = s->h; break;
    case EGL_CONFIG_ID: *value = s->cfg->id; break;
    case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; break;
    case EGL_SWAP_BEHAVIOR: *value = EGL_BUFFER_PRESERVED; break;
    case EGL_LARGEST_PBUFFER: case EGL_MIPMAP_TEXTURE: case EGL_MIPMAP_LEVEL: *value = 0; break;
    case EGL_TEXTURE_FORMAT: case EGL_TEXTURE_TARGET: *value = EGL_NO_TEXTURE; break;
    case EGL_HORIZONTAL_RESOLUTION: case EGL_VERTICAL_RESOLUTION: case EGL_PIXEL_ASPECT_RATIO: *value = EGL_UNKNOWN; break;
    case EGL_MULTISAMPLE_RESOLVE: *value = 0x309A /* EGL_MULTISAMPLE_RESOLVE_DEFAULT */; break;
    default: return fail(EGL_BAD_ATTRIBUTE, EGL_FALSE);
    }
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint, EGLint)
{
    if (!check_display(dpy)) return EGL_FALSE;
    if (!g_surfaces.count(static_cast<Surface *>(surface))) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglBindTexImage(EGLDisplay, EGLSurface, EGLint) { return fail(EGL_BAD_MATCH, EGL_FALSE); }
EGLAPI EGLBoolean EGLAPIENTRY eglReleaseTexImage(EGLDisplay, EGLSurface, EGLint) { return fail(EGL_BAD_MATCH, EGL_FALSE); }

EGLAPI EGLBoolean EGLAPIENTRY eglBindAPI(EGLenum api)
{
    if (api != EGL_OPENGL_ES_API) return fail(EGL_BAD_PARAMETER, EGL_FALSE);
    t_api = api;
    return EGL_TRUE;
}

EGLAPI EGLenum EGLAPIENTRY eglQueryAPI(void)
{
    return t_api;
}

EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                                              const EGLint *attrib_list)
{
    if (!check_display(dpy)) return EGL_NO_CONTEXT;
    const Config *c = config_of(config);
    if (!c && config != nullptr) return fail(EGL_BAD_CONFIG, EGL_NO_CONTEXT);
    int version = 1;
    for (const EGLint *a = attrib_list; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_CONTEXT_CLIENT_VERSION) version = a[1];
    }
    if (version > 3) return fail(EGL_BAD_MATCH, EGL_NO_CONTEXT);
    if (version < 2) version = 2;
    Ctx *share = static_cast<Ctx *>(share_context);
    if (share && !g_contexts.count(share)) return fail(EGL_BAD_CONTEXT, EGL_NO_CONTEXT);
    auto *ctx = new Ctx();
    ctx->version = version;
    ctx->cfg = c;
    ctx->gl = espgpu_gles_create_context(share ? share->gl : nullptr, version);
    g_contexts.insert(ctx);
    return ctx;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    if (!check_display(dpy)) return EGL_FALSE;
    Ctx *c = static_cast<Ctx *>(ctx);
    if (!g_contexts.count(c)) return fail(EGL_BAD_CONTEXT, EGL_FALSE);
    g_contexts.erase(c);
    if (t_ctx == c) {
        espgpu_gles_make_current(nullptr, nullptr, 0, 0);
        t_ctx = nullptr;
    }
    espgpu_gles_destroy_context(c->gl);
    delete c;
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    if (!check_display(dpy)) return EGL_FALSE;
    Ctx *c = static_cast<Ctx *>(ctx);
    Surface *d = static_cast<Surface *>(draw);
    Surface *r = static_cast<Surface *>(read);
    if (c && !g_contexts.count(c)) return fail(EGL_BAD_CONTEXT, EGL_FALSE);
    if ((d && !g_surfaces.count(d)) || (r && !g_surfaces.count(r))) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    if (!c && (d || r)) return fail(EGL_BAD_MATCH, EGL_FALSE);
    t_ctx = c;
    t_draw = d;
    t_read = r;
    espgpu_gles_make_current(c ? c->gl : nullptr, d ? d->hwnd : nullptr, d ? d->w : 1, d ? d->h : 1);
    return EGL_TRUE;
}

EGLAPI EGLContext EGLAPIENTRY eglGetCurrentContext(void)
{
    return t_ctx;
}

EGLAPI EGLSurface EGLAPIENTRY eglGetCurrentSurface(EGLint readdraw)
{
    return readdraw == EGL_READ ? t_read : t_draw;
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetCurrentDisplay(void)
{
    return t_ctx ? kDisplay : EGL_NO_DISPLAY;
}

EGLAPI EGLBoolean EGLAPIENTRY eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value)
{
    if (!check_display(dpy)) return EGL_FALSE;
    Ctx *c = static_cast<Ctx *>(ctx);
    if (!g_contexts.count(c)) return fail(EGL_BAD_CONTEXT, EGL_FALSE);
    switch (attribute) {
    case EGL_CONFIG_ID: *value = c->cfg ? c->cfg->id : 0; break;
    case EGL_CONTEXT_CLIENT_TYPE: *value = EGL_OPENGL_ES_API; break;
    case EGL_CONTEXT_CLIENT_VERSION: *value = c->version; break;
    case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; break;
    default: return fail(EGL_BAD_ATTRIBUTE, EGL_FALSE);
    }
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    if (!check_display(dpy)) return EGL_FALSE;
    Surface *s = static_cast<Surface *>(surface);
    if (!g_surfaces.count(s)) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    if (!t_ctx || t_draw != s) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    if (s->pbuffer) return EGL_TRUE;
    espgpu_gles_swap();
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
    if (!check_display(dpy)) return EGL_FALSE;
    if (!t_ctx) return fail(EGL_BAD_CONTEXT, EGL_FALSE);
    espgpu_gles_set_swap_interval(std::max(0, interval));
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglCopyBuffers(EGLDisplay, EGLSurface, EGLNativePixmapType)
{
    return fail(EGL_BAD_NATIVE_PIXMAP, EGL_FALSE);
}

EGLAPI EGLBoolean EGLAPIENTRY eglWaitGL(void) { return EGL_TRUE; }
EGLAPI EGLBoolean EGLAPIENTRY eglWaitNative(EGLint) { return EGL_TRUE; }
EGLAPI EGLBoolean EGLAPIENTRY eglWaitClient(void) { return EGL_TRUE; }

EGLAPI EGLBoolean EGLAPIENTRY eglReleaseThread(void)
{
    t_ctx = nullptr;
    t_draw = t_read = nullptr;
    espgpu_gles_make_current(nullptr, nullptr, 0, 0);
    t_error = EGL_SUCCESS;
    return EGL_TRUE;
}

EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char *procname)
{
    if (!procname) return nullptr;
    if (std::strncmp(procname, "egl", 3) == 0) {
        static HMODULE self = nullptr;
        if (!self) {
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&eglGetProcAddress), &self);
        }
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(GetProcAddress(self, procname));
    }
    return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(espgpu_gles_get_proc(procname));
}

} // extern "C"
