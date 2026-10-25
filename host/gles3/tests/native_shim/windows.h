/* 最小 Win32 替身：只为在 macOS / Linux 上本机编译 libGLESv2 源码做端到端测试（gles_e2e） */
#pragma once
#include <cstdint>
#include <cstddef>
#define __declspec(x)
#define WINAPI
typedef void *HWND;
typedef void *HDC;
typedef void *HMODULE;
typedef void *HINSTANCE;
typedef const char *LPCSTR;
typedef int (*FARPROC)(void);
typedef unsigned long DWORD;
typedef int BOOL;
typedef long LONG;
struct RECT { LONG left, top, right, bottom; };
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
inline BOOL GetClientRect(HWND, RECT *r) { r->left = r->top = 0; r->right = 320; r->bottom = 200; return 1; }
inline HDC GetDC(HWND) { return nullptr; }
inline int ReleaseDC(HWND, HDC) { return 1; }
inline void OutputDebugStringA(const char *) {}
inline BOOL GetModuleHandleExA(DWORD, LPCSTR, HMODULE *m) { *m = nullptr; return 0; }
inline FARPROC GetProcAddress(HMODULE, const char *) { return nullptr; }
inline BOOL IsWindow(HWND h) { return h != nullptr; }
