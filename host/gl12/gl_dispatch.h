/*
 * gl_dispatch.h — 导出包装共用的模式开关与 trace 钩子
 *
 * 三种模式（环境变量 ESPGPU_GL_MODE）：
 *   espgpu       （默认）在 ESP32-S3 GPU 上渲染
 *   trace        同 espgpu，另外记录每一次调用
 *   passthrough  全部转发给真实 GL（Mesa llvmpipe 或系统 opengl32），只统计调用清单
 *
 * trace / passthrough 模式下 g_watch 为真：每次调用计数、记录见过的枚举值，
 * 退出时（以及每 300 帧）写出 espgpu-gltrace.txt —— 这就是 PLAN R1 要的「精确 GL 调用清单」。
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdint>

#include "gen/gl_api.h"

namespace espgl {

enum Mode {
    MODE_ESPGPU = 0,
    MODE_PASSTHROUGH = 1,
};

extern int g_mode;         /**< Mode */
extern bool g_watch;       /**< 统计调用清单（trace / passthrough） */
extern bool g_trace_log;   /**< 逐调用日志（trace 模式，前 N 帧） */

void trace_count(FnId fn);
void trace_enum(FnId fn, int param, GLenum value);
void trace_call(FnId fn, const char *fmt, ...);
void trace_note(const char *fmt, ...);        /**< 写一行到 trace 日志（wgl 调用等） */
void trace_proc_request(const char *name, bool found);
void trace_frame();                            /**< SwapBuffers 时调用：帧计数、周期性落盘 */
void trace_dump();                             /**< 把调用清单写到 espgpu-gltrace.txt */

/** espgpu 模式下调用了尚未实现的入口：只记一次警告 */
void gl_unimplemented(FnId fn);

/** 从真实 GL 模块取符号 */
void *pt_sym(void *module, const char *name);
/** 扩展入口：经真实 wglGetProcAddress 懒解析，先试 name 再试 alias */
void *pt_ext(const char *name, const char *alias);

/** 真实 GL 模块（直通模式）及其 wgl 入口 */
struct PtWgl {
    HMODULE module = nullptr;
    HGLRC(WINAPI *CreateContext)(HDC) = nullptr;
    BOOL(WINAPI *DeleteContext)(HGLRC) = nullptr;
    BOOL(WINAPI *MakeCurrent)(HDC, HGLRC) = nullptr;
    HGLRC(WINAPI *GetCurrentContext)() = nullptr;
    HDC(WINAPI *GetCurrentDC)() = nullptr;
    PROC(WINAPI *GetProcAddress)(LPCSTR) = nullptr;
    BOOL(WINAPI *ShareLists)(HGLRC, HGLRC) = nullptr;
    BOOL(WINAPI *CopyContext)(HGLRC, HGLRC, UINT) = nullptr;
    BOOL(WINAPI *SwapBuffers)(HDC) = nullptr;
    BOOL(WINAPI *SwapLayerBuffers)(HDC, UINT) = nullptr;
    int(WINAPI *ChoosePixelFormat)(HDC, const PIXELFORMATDESCRIPTOR *) = nullptr;
    int(WINAPI *DescribePixelFormat)(HDC, int, UINT, LPPIXELFORMATDESCRIPTOR) = nullptr;
    int(WINAPI *GetPixelFormat)(HDC) = nullptr;
    BOOL(WINAPI *SetPixelFormat)(HDC, int, const PIXELFORMATDESCRIPTOR *) = nullptr;
    BOOL(WINAPI *UseFontBitmapsA)(HDC, DWORD, DWORD, DWORD) = nullptr;
    BOOL(WINAPI *UseFontBitmapsW)(HDC, DWORD, DWORD, DWORD) = nullptr;
    HGLRC(WINAPI *CreateLayerContext)(HDC, int) = nullptr;
};
extern PtWgl g_ptw;

/** 读取环境变量、决定模式；首次 wgl 调用前执行一次（线程安全） */
void mode_init();
/** 加载直通目标 DLL；失败返回 false */
bool pt_load();
/** 切到直通模式（espgpu 打不开设备时的回退） */
bool switch_to_passthrough(const char *why);

void log_line(const char *fmt, ...);

} // namespace espgl
