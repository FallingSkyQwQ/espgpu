/*
 * gl_trace.cpp — 模式选择、调用清单统计、直通目标加载
 */

#include "gl_dispatch.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace espgl {

int g_mode = MODE_ESPGPU;
bool g_watch = false;
bool g_trace_log = false;
PtWgl g_ptw;

namespace {

std::once_flag g_init_once;
std::mutex g_mu;

uint64_t g_counts[FN_COUNT];
std::set<uint64_t> g_enums;              /* (fn << 40) | (param << 32) | value */
std::map<std::string, std::pair<uint64_t, bool>> g_procs; /* wglGetProcAddress 请求 */
std::set<int> g_unimpl;
uint64_t g_frames = 0;
int g_trace_frames = 3;                   /* 逐调用日志记录的帧数 */
FILE *g_trace_file = nullptr;
FILE *g_log_file = nullptr;
std::string g_trace_path = "espgpu-gltrace.txt";
std::string g_calls_path = "espgpu-glcalls.log";

std::string env(const char *name)
{
    char buf[1024];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return std::string();
    }
    return std::string(buf, n);
}

const char *enum_name(uint32_t v)
{
    size_t lo = 0, hi = kEnumNameCount;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (kEnumNames[mid].value < v) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < kEnumNameCount && kEnumNames[lo].value == v) {
        return kEnumNames[lo].name;
    }
    return nullptr;
}

std::string exe_dir()
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path, n);
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? std::string(".") : s.substr(0, p);
}

void do_init()
{
    const std::string m = env("ESPGPU_GL_MODE");
    if (m == "trace") {
        g_mode = MODE_ESPGPU;
        g_watch = true;
        g_trace_log = true;
    } else if (m == "passthrough" || m == "mesa") {
        g_mode = MODE_PASSTHROUGH;
        g_watch = true;
        g_trace_log = env("ESPGPU_GL_TRACE_LOG") == "1";
    } else {
        g_mode = MODE_ESPGPU;
    }
    const std::string tf = env("ESPGPU_GL_TRACE_FRAMES");
    if (!tf.empty()) {
        g_trace_frames = atoi(tf.c_str());
    }
    const std::string out = env("ESPGPU_GL_TRACE_FILE");
    if (!out.empty()) {
        g_trace_path = out;
    }
    if (g_watch || env("ESPGPU_GL_LOG") == "1") {
        g_log_file = fopen("espgpu-gl.log", "w");
    }
    if (g_mode == MODE_PASSTHROUGH && !pt_load()) {
        log_line("passthrough target not found; falling back to espgpu mode");
        g_mode = MODE_ESPGPU;
    }
    log_line("opengl32 (espgpu) mode=%s watch=%d", g_mode == MODE_PASSTHROUGH ? "passthrough" : "espgpu",
             g_watch ? 1 : 0);
}

} // namespace

void log_line(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA("[espgpu-gl] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_log_file) {
        fprintf(g_log_file, "%s\n", buf);
        fflush(g_log_file);
    }
}

void mode_init()
{
    std::call_once(g_init_once, do_init);
}

void trace_count(FnId fn)
{
    g_counts[fn]++;
}

void trace_enum(FnId fn, int param, GLenum value)
{
    const uint64_t key = (static_cast<uint64_t>(fn) << 40) | (static_cast<uint64_t>(param) << 32) | value;
    std::lock_guard<std::mutex> lk(g_mu);
    g_enums.insert(key);
}

void trace_call(FnId fn, const char *fmt, ...)
{
    (void)fn;
    if (g_trace_frames >= 0 && g_frames >= static_cast<uint64_t>(g_trace_frames)) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_trace_file == nullptr) {
        g_trace_file = fopen(g_calls_path.c_str(), "w");
        if (g_trace_file == nullptr) {
            return;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_trace_file, fmt, ap);
    va_end(ap);
    fputc('\n', g_trace_file);
}

void trace_note(const char *fmt, ...)
{
    if (!g_watch) {
        return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_trace_log) {
        trace_call(FN_COUNT, "%s", buf);
    }
    log_line("%s", buf);
}

void trace_proc_request(const char *name, bool found)
{
    if (!g_watch || name == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto &e = g_procs[name];
    e.first++;
    e.second = found;
}

void trace_frame()
{
    g_frames++;
    if (g_trace_file && g_trace_frames >= 0 && g_frames == static_cast<uint64_t>(g_trace_frames)) {
        std::lock_guard<std::mutex> lk(g_mu);
        fprintf(g_trace_file, "---- per-call log stopped after %d frame(s) ----\n", g_trace_frames);
        fflush(g_trace_file);
    } else if (g_trace_file) {
        std::lock_guard<std::mutex> lk(g_mu);
        fprintf(g_trace_file, "---- frame %llu ----\n", static_cast<unsigned long long>(g_frames));
    }
    if (g_watch && g_frames % 300 == 0) {
        trace_dump();
    }
}

void trace_dump()
{
    if (!g_watch) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    FILE *f = fopen(g_trace_path.c_str(), "w");
    if (f == nullptr) {
        return;
    }
    fprintf(f, "# espgpu GL call inventory\n");
    fprintf(f, "# mode: %s   frames: %llu\n", g_mode == MODE_PASSTHROUGH ? "passthrough" : "espgpu",
            static_cast<unsigned long long>(g_frames));
    fprintf(f, "# columns: calls  calls/frame  function\n\n");

    std::vector<std::pair<uint64_t, int>> v;
    for (int i = 0; i < FN_COUNT; i++) {
        if (g_counts[i]) {
            v.emplace_back(g_counts[i], i);
        }
    }
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
    fprintf(f, "## functions (%zu unique)\n", v.size());
    for (auto &e : v) {
        fprintf(f, "%12llu %10.1f  %s%s\n", static_cast<unsigned long long>(e.first),
                g_frames ? static_cast<double>(e.first) / static_cast<double>(g_frames) : 0.0,
                kFnNames[e.second], kFnIsExt[e.second] ? "  (ext)" : "");
    }

    fprintf(f, "\n## enum arguments seen\n");
    int last_fn = -1;
    int last_param = -1;
    for (uint64_t key : g_enums) {
        const int fn = static_cast<int>(key >> 40);
        const int param = static_cast<int>((key >> 32) & 0xFF);
        const uint32_t value = static_cast<uint32_t>(key & 0xFFFFFFFFu);
        if (fn != last_fn || param != last_param) {
            fprintf(f, "%s  arg%d:\n", fn < FN_COUNT ? kFnNames[fn] : "?", param);
            last_fn = fn;
            last_param = param;
        }
        const char *n = enum_name(value);
        fprintf(f, "    0x%04X %s\n", value, n ? n : "");
    }

    fprintf(f, "\n## wglGetProcAddress requests\n");
    for (auto &e : g_procs) {
        fprintf(f, "%8llu  %-40s %s\n", static_cast<unsigned long long>(e.second.first), e.first.c_str(),
                e.second.second ? "provided" : "NOT PROVIDED");
    }

    if (!g_unimpl.empty()) {
        fprintf(f, "\n## entry points hit while unimplemented in espgpu mode\n");
        for (int fn : g_unimpl) {
            fprintf(f, "    %s\n", kFnNames[fn]);
        }
    }
    fclose(f);
}

void gl_unimplemented(FnId fn)
{
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        first = g_unimpl.insert(fn).second;
    }
    if (first) {
        log_line("unimplemented in espgpu mode: %s", kFnNames[fn]);
    }
}

void *pt_sym(void *module, const char *name)
{
    if (module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(module), name));
}

void *pt_ext(const char *name, const char *alias)
{
    if (g_ptw.GetProcAddress == nullptr) {
        return nullptr;
    }
    void *p = reinterpret_cast<void *>(g_ptw.GetProcAddress(name));
    if (p == nullptr && alias && strcmp(alias, name) != 0) {
        p = reinterpret_cast<void *>(g_ptw.GetProcAddress(alias));
    }
    if (p == nullptr) {
        /* 有的实现把 1.1 之后的核心入口也直接导出 */
        p = pt_sym(g_ptw.module, name);
    }
    return p;
}

bool pt_load()
{
    if (g_ptw.module) {
        return true;
    }
    std::vector<std::string> cands;
    const std::string e = env("ESPGPU_GL_PASSTHROUGH_DLL");
    if (!e.empty()) {
        cands.push_back(e);
    }
    cands.push_back(exe_dir() + "\\opengl32_mesa.dll");
    char sys[MAX_PATH];
    UINT n = GetSystemDirectoryA(sys, MAX_PATH);
    if (n) {
        cands.push_back(std::string(sys, n) + "\\opengl32.dll");
    }

    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&pt_load), &self);

    for (const auto &c : cands) {
        HMODULE m = LoadLibraryA(c.c_str());
        if (m == nullptr || m == self) {
            continue;
        }
        g_ptw.module = m;
        log_line("passthrough target: %s", c.c_str());
        break;
    }
    if (g_ptw.module == nullptr) {
        return false;
    }

#define LOADW(field, sym) g_ptw.field = reinterpret_cast<decltype(g_ptw.field)>(GetProcAddress(g_ptw.module, sym))
    LOADW(CreateContext, "wglCreateContext");
    LOADW(DeleteContext, "wglDeleteContext");
    LOADW(MakeCurrent, "wglMakeCurrent");
    LOADW(GetCurrentContext, "wglGetCurrentContext");
    LOADW(GetCurrentDC, "wglGetCurrentDC");
    LOADW(GetProcAddress, "wglGetProcAddress");
    LOADW(ShareLists, "wglShareLists");
    LOADW(CopyContext, "wglCopyContext");
    LOADW(SwapBuffers, "wglSwapBuffers");
    LOADW(SwapLayerBuffers, "wglSwapLayerBuffers");
    LOADW(ChoosePixelFormat, "wglChoosePixelFormat");
    LOADW(DescribePixelFormat, "wglDescribePixelFormat");
    LOADW(GetPixelFormat, "wglGetPixelFormat");
    LOADW(SetPixelFormat, "wglSetPixelFormat");
    LOADW(UseFontBitmapsA, "wglUseFontBitmapsA");
    LOADW(UseFontBitmapsW, "wglUseFontBitmapsW");
    LOADW(CreateLayerContext, "wglCreateLayerContext");
#undef LOADW

    pt_load_core(g_ptw.module);
    return true;
}

bool switch_to_passthrough(const char *why)
{
    log_line("switching to passthrough: %s", why ? why : "");
    if (!pt_load()) {
        log_line("no passthrough GL available");
        return false;
    }
    g_mode = MODE_PASSTHROUGH;
    return true;
}

} // namespace espgl
