/*
 * winusb.cpp — 设备枚举与原始 bulk 收发
 *
 * 枚举方式：遍历 USB 设备接口，按硬件 ID 里的 VID/PID 匹配。
 * 这样不依赖 INF 里写的 DeviceInterfaceGUID，WCID 免驱设备也能找到。
 */

#include "internal.h"

#include <setupapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")

namespace espgpu {

/* {A5DCBF10-6530-11D2-901F-00C04FB951ED} — USB 设备接口类 */
static const GUID kUsbDeviceInterfaceGuid = {
    0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

namespace {

/* 一个带事件的 OVERLAPPED，用于可超时的同步等待 */
struct IoCtx {
    OVERLAPPED ov{};
    HANDLE ev;

    IoCtx()
    {
        ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ov.hEvent = ev;
    }
    ~IoCtx()
    {
        if (ev) {
            CloseHandle(ev);
        }
    }
    IoCtx(const IoCtx &) = delete;
    IoCtx &operator=(const IoCtx &) = delete;
};

bool do_io(espgpu_device *dev, UCHAR pipe, void *buf, DWORD len, bool is_read,
           DWORD timeout_ms, DWORD *transferred, bool *timed_out)
{
    *timed_out = false;
    IoCtx ctx;
    if (ctx.ev == nullptr) {
        return false;
    }

    BOOL ok = is_read ? WinUsb_ReadPipe(dev->winusb, pipe, static_cast<PUCHAR>(buf), len,
                                        nullptr, &ctx.ov)
                      : WinUsb_WritePipe(dev->winusb, pipe, static_cast<PUCHAR>(buf), len,
                                         nullptr, &ctx.ov);

    if (!ok) {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            log_msg(LOG_DEBUG, "WinUsb_%sPipe failed: %lu", is_read ? "Read" : "Write", e);
            return false;
        }
        DWORD w = WaitForSingleObject(ctx.ev, timeout_ms);
        if (w != WAIT_OBJECT_0) {
            /* 超时：必须 abort 才能取消这个 IRP，否则管道会一直挂着 */
            WinUsb_AbortPipe(dev->winusb, pipe);
            DWORD dummy = 0;
            WinUsb_GetOverlappedResult(dev->winusb, &ctx.ov, &dummy, TRUE);
            *timed_out = true;
            return false;
        }
    }

    DWORD n = 0;
    if (!WinUsb_GetOverlappedResult(dev->winusb, &ctx.ov, &n, FALSE)) {
        return false;
    }
    *transferred = n;
    return true;
}

} // namespace

/* ── 枚举 ─────────────────────────────────────────────────────────── */

bool usb_enumerate(std::vector<std::string> &paths)
{
    paths.clear();

    HDEVINFO set = SetupDiGetClassDevsW(&kUsbDeviceInterfaceGuid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        log_msg(LOG_ERROR, "SetupDiGetClassDevs failed: %lu", GetLastError());
        return false;
    }

    char want_vidpid[32];
    snprintf(want_vidpid, sizeof(want_vidpid), "VID_%04X&PID_%04X", ESPGPU_USB_VID,
             ESPGPU_USB_PID);

    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &kUsbDeviceInterfaceGuid, i, &did)) {
            break; /* ERROR_NO_MORE_ITEMS */
        }

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &did, nullptr, 0, &need, nullptr);
        if (need == 0) {
            continue;
        }

        std::vector<uint8_t> buf(need);
        auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devinfo{};
        devinfo.cbSize = sizeof(devinfo);

        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, need, nullptr, &devinfo)) {
            continue;
        }

        /* 用硬件 ID 过滤，避免误开别的 USB 设备 */
        char hwid[512] = {0};
        DWORD type = 0;
        DWORD hwlen = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &devinfo, SPDRP_HARDWAREID, &type,
                                               reinterpret_cast<PBYTE>(hwid), sizeof(hwid),
                                               &hwlen)) {
            continue;
        }

        /* 多字符串，逐个比较 */
        for (const WCHAR *p = reinterpret_cast<const WCHAR *>(hwid);
             *p; p += wcslen(p) + 1) {
            char narrow[512] = {0};
            WideCharToMultiByte(CP_ACP, 0, p, -1, narrow, sizeof(narrow), nullptr, nullptr);
            if (strstr(narrow, want_vidpid) != nullptr) {
                /* detail->DevicePath 是宽字符，转成 UTF-8 便于日志与界面显示 */
                char utf8[1024] = {0};
                WideCharToMultiByte(CP_UTF8, 0, detail->DevicePath, -1, utf8, sizeof(utf8),
                                    nullptr, nullptr);
                paths.emplace_back(utf8);
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(set);
    return !paths.empty();
}

/* ── 打开 / 关闭 ──────────────────────────────────────────────────── */

bool usb_open(espgpu_device *dev, const std::string &path, std::string &err)
{
    /* 路径是 UTF-8，转回宽字符给 CreateFileW */
    WCHAR wpath[1024] = {0};
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, 1024);

    HANDLE h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        char buf[128];
        snprintf(buf, sizeof(buf), "CreateFile failed: %lu", GetLastError());
        err = buf;
        return false;
    }

    WINUSB_INTERFACE_HANDLE wu = nullptr;
    if (!WinUsb_Initialize(h, &wu)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "WinUsb_Initialize failed: %lu (驱动不是 WinUSB?)",
                 GetLastError());
        CloseHandle(h);
        err = buf;
        return false;
    }

    USB_INTERFACE_DESCRIPTOR ifd{};
    if (!WinUsb_QueryInterfaceSettings(wu, 0, &ifd)) {
        err = "WinUsb_QueryInterfaceSettings failed";
        WinUsb_Free(wu);
        CloseHandle(h);
        return false;
    }

    UCHAR ep_in = 0;
    UCHAR ep_out = 0;
    for (UCHAR i = 0; i < ifd.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pi{};
        if (!WinUsb_QueryPipe(wu, 0, i, &pi)) {
            continue;
        }
        if (pi.PipeType != UsbdPipeTypeBulk) {
            continue;
        }
        if (pi.PipeId & 0x80) {
            ep_in = pi.PipeId;
        } else {
            ep_out = pi.PipeId;
        }
    }

    if (ep_in == 0 || ep_out == 0) {
        err = "bulk endpoints not found";
        WinUsb_Free(wu);
        CloseHandle(h);
        return false;
    }

    /* 管道策略：读超时交给 OVERLAPPED 控制；禁止自动清 stall，
     * 让上层能看到错误并决定是否 reset。 */
    UCHAR enable = 0;
    WinUsb_SetPipePolicy(wu, ep_in, AUTO_CLEAR_STALL, sizeof(enable), &enable);
    WinUsb_SetPipePolicy(wu, ep_out, AUTO_CLEAR_STALL, sizeof(enable), &enable);

    dev->file = h;
    dev->winusb = wu;
    dev->ep_in = ep_in;
    dev->ep_out = ep_out;
    dev->path = path;
    dev->connected = true;

    log_msg(LOG_INFO, "opened %s (EP OUT 0x%02X, EP IN 0x%02X)", path.c_str(), ep_out, ep_in);
    return true;
}

void usb_close(espgpu_device *dev)
{
    if (dev->winusb) {
        WinUsb_AbortPipe(dev->winusb, dev->ep_in);
        WinUsb_AbortPipe(dev->winusb, dev->ep_out);
        WinUsb_Free(dev->winusb);
        dev->winusb = nullptr;
    }
    if (dev->file != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->file);
        dev->file = INVALID_HANDLE_VALUE;
    }
    dev->connected = false;
}

/* ── 收发 ─────────────────────────────────────────────────────────── */

bool usb_write(espgpu_device *dev, const void *data, size_t len, DWORD timeout_ms)
{
    if (!dev->connected || dev->winusb == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lk(dev->write_mu);

    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t done = 0;
    while (done < len) {
        DWORD chunk = static_cast<DWORD>(len - done);
        DWORD n = 0;
        bool timed_out = false;
        if (!do_io(dev, dev->ep_out, const_cast<uint8_t *>(p + done), chunk, false,
                   timeout_ms, &n, &timed_out)) {
            if (!timed_out) {
                log_msg(LOG_WARN, "bulk OUT failed at %zu/%zu", done, len);
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        done += n;
        std::lock_guard<std::mutex> slk(dev->stats_mu);
        dev->stats.bytes_out += n;
        dev->win_bytes_out += n;
    }
    return true;
}

int usb_read(espgpu_device *dev, void *buf, DWORD cap, DWORD timeout_ms, DWORD *got)
{
    if (!dev->connected || dev->winusb == nullptr) {
        return -1;
    }
    DWORD n = 0;
    bool timed_out = false;
    if (!do_io(dev, dev->ep_in, buf, cap, true, timeout_ms, &n, &timed_out)) {
        return timed_out ? 0 : -1;
    }
    *got = n;
    if (n) {
        std::lock_guard<std::mutex> lk(dev->stats_mu);
        dev->stats.bytes_in += n;
        dev->win_bytes_in += n;
    }
    return 1;
}

} // namespace espgpu
