/*
 * app.h — 面板的全局状态
 *
 * 线程模型：
 *   - UI 线程：Win32 消息、ImGui、D3D11。只做非阻塞调用
 *     （espgpu_frame_snapshot / espgpu_link_stats / espgpu_cached_info），
 *     且必须在持有 dev_mu 时调用，防止设备在别的线程上被关闭。
 *   - 设备线程（worker）：连接/重连、espgpu_poll、统计轮询、事件、以及所有
 *     同步控制请求（set_mode / set_clock / OTA…）。UI 通过 enqueue() 投递任务。
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "espgpu/espgpu.h"
#include "imgui.h"

namespace panel {

/* ── 页面 ─────────────────────────────────────────────────────────── */

enum Page {
    PAGE_OVERVIEW = 0,
    PAGE_DISPLAY,
    PAGE_PERFORMANCE,
    PAGE_VIEWER,
    PAGE_DEBUG,
    PAGE_MEMORY,
    PAGE_CLOCK,
    PAGE_FIRMWARE,
    PAGE_LOG,
    PAGE_APPEARANCE,
    PAGE_COUNT,
};

/* ── 连接状态 ─────────────────────────────────────────────────────── */

enum class Conn {
    Searching,  /* 还没尝试过 */
    NotFound,   /* 没有枚举到设备 */
    Busy,       /* 枚举到了但打不开（通常是被 GL 程序独占） */
    Connected,
};

/* ── 日志 ─────────────────────────────────────────────────────────── */

struct LogLine {
    int level;       /* 1 error 2 warn 3 info 4 debug */
    bool device;     /* 来自设备的 LOG chunk */
    double time;     /* 面板启动后的秒数 */
    std::string text;
};

/* ── 历史曲线（环形缓冲） ─────────────────────────────────────────── */

constexpr size_t kLogMax = 5000; /* 日志行数上限（超出丢最旧） */

constexpr int kHistLen = 240; /* 200ms 一个点 = 48 秒 */

struct History {
    float fps[kHistLen]{};
    float in_kbps[kHistLen]{};
    float out_kbps[kHistLen]{};
    float temp_c[kHistLen]{};
    float frame_ms[kHistLen]{};
    int pos = 0; /* 下一个写入位置 = 最旧的点 */
};

/* ── 显示设置草稿 ─────────────────────────────────────────────────── */

struct ModeDraft {
    espgpu_mode_t mode{};
    int pattern = 2;
    int render_codec = ESPGPU_CODEC_DELTA_RLE;
    bool dirty = false;
};

/* ── OTA 进度 ─────────────────────────────────────────────────────── */

struct OtaState {
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::atomic<uint32_t> done{0};
    std::atomic<uint32_t> total{0};
    std::atomic<int> result{0};      /* 0 = 进行中/未开始, 1 = 成功, -1 = 失败, -2 = 取消 */
    std::atomic<bool> needs_reboot{false};
    std::wstring file_path;          /* 仅 UI 线程访问 */
    std::vector<uint8_t> image;      /* UI 线程准备好后交给 worker，运行中不可改 */
    std::string version;
};

/* ── 面板设置（持久化到 espgpu-panel.cfg） ────────────────────────── */

struct Settings {
    int theme_mode = 0;     /* 0 dark 1 light 2 follow system */
    bool reduce_motion = false;
    int seed = 0;
    bool viewer_integer = false;
    bool viewer_hud = true;
    bool device_log = false;
    int log_level_mask = 0x0F; /* bit (level-1) */
    int page = PAGE_OVERVIEW;
};

/* ── 全局状态 ─────────────────────────────────────────────────────── */

struct App {
    /* Win32 / D3D */
    HWND hwnd = nullptr;
    ID3D11Device *d3d = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    float dpi_scale = 1.0f;
    float pending_dpi = 0.0f; /* WM_DPICHANGED 后在下一帧开头重建字体 */
    bool fullscreen = false;
    WINDOWPLACEMENT saved_placement{};
    LONG_PTR saved_style = 0;

    /* 设备（dev 指针只由 worker 改写，改写时持有 dev_mu） */
    std::mutex dev_mu;
    espgpu_device_t *dev = nullptr;
    std::atomic<Conn> conn{Conn::Searching};
    std::mutex status_mu;
    std::string status_text = "Searching for device…";
    std::string device_path_;    /* 受 status_mu 保护，用 device_path() 读 */
    std::atomic<bool> emulator{false};
    std::atomic<bool> new_frame{false};
    std::atomic<double> last_frame_time{-100.0};
    std::atomic<uint32_t> conn_gen{0};   /* 每次连上 +1，页面据此丢弃上一连接的缓存状态 */
    std::atomic<bool> want_device_log{false}; /* settings.device_log 的线程安全镜像 */
    std::atomic<bool> viewer_visible{false};  /* Viewer 页 / 全屏可见：只有这时新帧才唤醒 UI */
    HANDLE wake = nullptr;                     /* worker → UI 的唤醒事件（自动复位） */

    /* 设备信息（连接时读一次） */
    std::mutex info_mu;
    espgpu_device_info_t info{};
    bool have_info = false;

    /* 统计快照（worker 写，UI 读） */
    std::mutex stats_mu;
    espgpu_stats_t stats{};
    bool stats_valid = false;
    espgpu_link_stats_t link{};
    History hist;
    uint32_t evictions_seen = 0; /* EVT_TEXTURE_EVICTED 计数 */

    /* 任务队列 */
    std::mutex job_mu;
    std::deque<std::function<void(espgpu_device_t *)>> jobs;

    /* worker */
    std::atomic<bool> running{true};

    /* 日志（最多 kLogMax 行；log_seq = 累计写入行数，log[0] 的序号 = log_seq - log.size()） */
    std::mutex log_mu;
    std::deque<LogLine> log;
    uint64_t log_seq = 0;
    uint64_t log_clear_gen = 0; /* Clear 之后 +1，日志页据此重建过滤缓存 */

    /* UI 状态 */
    Settings settings;
    ModeDraft draft;
    OtaState ota;
    int page = PAGE_OVERVIEW;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
};

extern App g_app;

/* ── app.cpp 提供 ─────────────────────────────────────────────────── */

double now_s();

/** 线程安全地追加一行面板日志 */
void log_line(int level, bool device, const char *fmt, ...);

/**
 * 投递一个同步控制任务给设备线程。fn 返回 0 视为成功；
 * ok_msg 非空时成功后弹 snackbar，失败时弹 "<what> failed (status N)"。
 */
void enqueue(const char *what, std::function<int(espgpu_device_t *)> fn,
             const char *ok_msg = nullptr);

/** 底层投递：fn 在设备线程上执行，未连接时 dev == nullptr（fn 自己负责收尾）。 */
void enqueue_raw(std::function<void(espgpu_device_t *)> fn);

/** 设备是否可用（UI 侧判断用） */
bool connected();

/** 状态文本（线程安全拷贝） */
std::string status_text();
/** 当前设备路径（线程安全拷贝） */
std::string device_path();

void save_settings();
void apply_theme_settings();
void toggle_fullscreen();

/** 统计快照的线程安全拷贝 */
struct Snapshot {
    bool valid = false;
    espgpu_stats_t s{};
    espgpu_link_stats_t link{};
    espgpu_device_info_t info{};
    bool have_info = false;
};
Snapshot snapshot();

/* ── viewer.cpp 提供 ──────────────────────────────────────────────── */

bool viewer_init();
void viewer_shutdown();
/** 有新帧时把它上传到 D3D 纹理（UI 线程调用） */
void viewer_upload();
/** 画帧：按 fit / integer 缩放居中到 avail 区域 */
void viewer_draw(ImVec2 avail, bool hud);
bool viewer_has_frame();
int viewer_width();
int viewer_height();
/** 截图保存为 BMP，返回路径（失败返回空） */
std::wstring viewer_screenshot();

/* ── pages/ 下各文件提供 ─────────────────────────────────────────────── */

void page_overview();
void page_display();
void page_performance();
void page_viewer();
void page_debug();
void page_memory();
void page_clock();
void page_firmware();
void page_log();
void page_appearance();

/** 未连接时各设备页面共用的空状态。返回 true = 已连接，页面可继续画。 */
bool require_device();

} // namespace panel
