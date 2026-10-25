/* 极简 windows.h 替身：只够把 host/gl12 在 macOS 上编译成本地测试程序 */
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <pthread.h>
#include <unistd.h>
typedef int BOOL; typedef unsigned long DWORD; typedef unsigned int UINT; typedef long LONG; typedef long long LONGLONG;
typedef void *HANDLE; typedef struct HDC__ *HDC; typedef struct HWND__ *HWND; typedef struct HGLRC__ *HGLRC; typedef void *HMODULE; typedef void *HINSTANCE; typedef void *LPVOID;
typedef const char *LPCSTR; typedef intptr_t (*PROC)(); typedef float FLOAT; typedef unsigned long COLORREF; typedef unsigned char BYTE; typedef unsigned short WORD;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define DLL_PROCESS_DETACH 0
#define ERROR_INVALID_PIXEL_FORMAT 2000
#define ERROR_INVALID_HANDLE 6
#define ERROR_BUSY 170
typedef struct { LONG left, top, right, bottom; } RECT;
typedef union { struct { DWORD LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;
inline int &shim_client_w() { static int w = 640; return w; }
inline int &shim_client_h() { static int h = 400; return h; }
inline DWORD GetEnvironmentVariableA(const char *n, char *b, DWORD cap) { const char *v = getenv(n); if (!v) return 0; size_t l = strlen(v); if (l >= cap) return (DWORD)l + 1; memcpy(b, v, l + 1); return (DWORD)l; }
inline void OutputDebugStringA(const char *s) { if (getenv("SHIM_LOG")) fputs(s, stderr); }
inline BOOL GetClientRect(HWND, RECT *r) { r->left = r->top = 0; r->right = shim_client_w(); r->bottom = shim_client_h(); return 1; }
inline BOOL IsIconic(HWND) { return shim_client_w() == 0; }
inline HDC GetDC(HWND w) { return (HDC)w; }
inline int ReleaseDC(HWND, HDC) { return 1; }
inline HWND WindowFromDC(HDC d) { return (HWND)d; }
inline void Sleep(DWORD) {}
inline void SetLastError(DWORD) {}
inline DWORD GetCurrentThreadId() { return (DWORD)(uintptr_t)pthread_self(); }
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *l) { l->QuadPart = 1000000; return 1; }
inline BOOL QueryPerformanceCounter(LARGE_INTEGER *l) { static LONGLONG t = 0; t += 1000; l->QuadPart = t; return 1; }
inline HMODULE LoadLibraryA(const char *) { return nullptr; }
inline void *GetProcAddress(HMODULE, const char *) { return nullptr; }
inline DWORD GetModuleFileNameA(HMODULE, char *b, DWORD) { strcpy(b, "x.exe"); return 5; }
inline UINT GetSystemDirectoryA(char *b, UINT) { strcpy(b, "C:"); return 2; }
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
inline BOOL GetModuleHandleExA(DWORD, const char *, HMODULE *m) { *m = nullptr; return 1; }
#define PFD_DOUBLEBUFFER 1
#define PFD_DRAW_TO_WINDOW 4
#define PFD_SUPPORT_OPENGL 0x20
#define PFD_GENERIC_ACCELERATED 0x1000
#define PFD_DOUBLEBUFFER_DONTCARE 0x40000000
#define PFD_TYPE_RGBA 0
#define PFD_MAIN_PLANE 0
typedef struct { WORD nSize, nVersion; DWORD dwFlags; BYTE iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift, cBlueBits, cBlueShift, cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits, cStencilBits, cAuxBuffers, iLayerType, bReserved; DWORD dwLayerMask, dwVisibleMask, dwDamageMask; } PIXELFORMATDESCRIPTOR, *LPPIXELFORMATDESCRIPTOR;
typedef struct { int x; } *LPGLYPHMETRICSFLOAT, *LPLAYERPLANEDESCRIPTOR;
typedef struct { HDC hdc; UINT uiFlags; } WGLSWAP;
