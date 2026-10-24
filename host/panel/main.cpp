/*
 * espgpu-panel — NVIDIA 控制面板风格的控制台 + 帧查看器
 *
 * 纯 Win32 + Direct3D 11 + Dear ImGui，不引入 GLFW/SDL。
 * 没有物理屏幕，所以 "帧查看器" 页面就是这台 GPU 的显示器。
 */

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "espgpu/espgpu.h"

#pragma comment(lib, "d3d11.lib")

/* ── 版本 ─────────────────────────────────────────────────────────── */

#define ESPGPU_PANEL_VERSION "0.1.0"

/* ── D3D11 ────────────────────────────────────────────────────────── */

static ID3D11Device *g_d3d = nullptr;
static ID3D11DeviceContext *g_ctx = nullptr;
static IDXGISwapChain *g_swap = nullptr;
static ID3D11RenderTargetView *g_rtv = nullptr;
static bool g_swap_occluded = false;

static ID3D11ShaderResourceView *g_frame_srv = nullptr;
static int g_tex_w = 0;
static int g_tex_h = 0;

/* ── 应用状态 ─────────────────────────────────────────────────────── */

namespace {

struct LogLine {
    int level;
    std::string text;
};

std::mutex g_log_mu;
std::deque<LogLine> g_log;
bool g_log_autoscroll = true;

espgpu_device_t *g_dev = nullptr;
espgpu_device_info_t g_info{};
espgpu_stats_t g_dev_stats{};
bool g_have_info = false;
bool g_stats_valid = false;
std::chrono::steady_clock::time_point g_last_stats{};

std::chrono::steady_clock::time_point g_last_connect_attempt{};
constexpr auto kReconnectInterval = std::chrono::milliseconds(1500);

/* 历史曲线 */
constexpr int kHistLen = 240;
float g_hist_fps[kHistLen]{};
float g_hist_in[kHistLen]{};
float g_hist_out[kHistLen]{};
int g_hist_pos = 0;
std::chrono::steady_clock::time_point g_last_hist{};

/* 显示设置草稿 */
espgpu_mode_t g_mode_draft{};
int g_pattern_draft = 2;
bool g_mode_dirty = false;

int g_page = 0;
char g_status[256] = "未连接";

} // namespace

/* ── 日志 ─────────────────────────────────────────────────────────── */

static void log_cb(int level, const char *msg, void *user)
{
    (void)user;
    std::lock_guard<std::mutex> lk(g_log_mu);
    g_log.push_back({level, msg});
    while (g_log.size() > 2000) {
        g_log.pop_front();
    }
}

static void push_ui_log(int level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_log_mu);
    g_log.push_back({level, buf});
    while (g_log.size() > 2000) {
        g_log.pop_front();
    }
}

/* ── D3D11 初始化 ─────────────────────────────────────────────────── */

static void create_rtv()
{
    ID3D11Texture2D *back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_d3d->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void release_rtv()
{
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

static bool init_d3d(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swap, &g_d3d, &got, &g_ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
            &g_swap, &g_d3d, &got, &g_ctx);
    }
    if (FAILED(hr)) {
        return false;
    }

    create_rtv();
    return true;
}

static void cleanup_d3d()
{
    if (g_frame_srv) {
        g_frame_srv->Release();
        g_frame_srv = nullptr;
    }
    release_rtv();
    if (g_swap) {
        g_swap->Release();
        g_swap = nullptr;
    }
    if (g_ctx) {
        g_ctx->Release();
        g_ctx = nullptr;
    }
    if (g_d3d) {
        g_d3d->Release();
        g_d3d = nullptr;
    }
}

/* ── 帧纹理上传 ───────────────────────────────────────────────────── */

static std::vector<uint8_t> g_frame_copy;

static void upload_frame()
{
    if (!g_dev) {
        return;
    }
    const int w0 = espgpu_frame_width(g_dev);
    const int h0 = espgpu_frame_height(g_dev);
    if (w0 <= 0 || h0 <= 0) {
        return;
    }

    const size_t need = static_cast<size_t>(w0) * h0 * 4;
    if (g_frame_copy.size() < need) {
        g_frame_copy.resize(need);
    }

    int w = 0;
    int h = 0;
    uint32_t id = 0;
    if (espgpu_frame_snapshot(g_dev, g_frame_copy.data(), g_frame_copy.size(), &w, &h, &id) !=
        0) {
        return;
    }
    const uint8_t *rgba = g_frame_copy.data();
    (void)id;

    if (g_frame_srv == nullptr || g_tex_w != w || g_tex_h != h) {
        if (g_frame_srv) {
            g_frame_srv->Release();
            g_frame_srv = nullptr;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        ID3D11Texture2D *tex = nullptr;
        if (FAILED(g_d3d->CreateTexture2D(&desc, nullptr, &tex)) || tex == nullptr) {
            return;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;

        HRESULT hr = g_d3d->CreateShaderResourceView(tex, &srv, &g_frame_srv);
        tex->Release();
        if (FAILED(hr)) {
            g_frame_srv = nullptr;
            return;
        }
        g_tex_w = w;
        g_tex_h = h;
    }

    ID3D11Resource *res = nullptr;
    g_frame_srv->GetResource(&res);
    if (res == nullptr) {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(g_ctx->Map(res, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        const size_t row_bytes = static_cast<size_t>(w) * 4;
        for (int y = 0; y < h; y++) {
            memcpy(static_cast<uint8_t *>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                   rgba + static_cast<size_t>(y) * row_bytes, row_bytes);
        }
        g_ctx->Unmap(res, 0);
    }
    res->Release();
}

/* ── 设备连接 ─────────────────────────────────────────────────────── */

static void try_connect()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_connect_attempt < kReconnectInterval) {
        return;
    }
    g_last_connect_attempt = now;

    if (g_dev && !espgpu_is_connected(g_dev)) {
        espgpu_close(g_dev);
        g_dev = nullptr;
        g_have_info = false;
        snprintf(g_status, sizeof(g_status), "设备已断开，正在重连…");
        push_ui_log(2, "device lost");
    }

    if (g_dev) {
        return;
    }

    if (espgpu_device_count() <= 0) {
        snprintf(g_status, sizeof(g_status), "未找到 ESP32-S3 GPU（检查 USB 线是否插在原生 USB 口）");
        return;
    }

    char err[256] = {0};
    g_dev = espgpu_open(0, err, sizeof(err));
    if (!g_dev) {
        snprintf(g_status, sizeof(g_status), "打开失败: %s", err);
        push_ui_log(1, "open failed: %s", err);
        return;
    }

    snprintf(g_status, sizeof(g_status), "已连接");
    g_have_info = espgpu_get_info(g_dev, &g_info, 1500) == 0;

    g_mode_draft.width = 320;
    g_mode_draft.height = 200;
    g_mode_draft.format = ESPGPU_FMT_RGB565;
    g_mode_draft.codec = ESPGPU_CODEC_DELTA_RLE;
    g_mode_draft.max_fps = 24;
    g_mode_draft.vsync = 1;
    g_pattern_draft = 2;
    g_mode_dirty = false;

    push_ui_log(3, "connected to %s", espgpu_device_path(g_dev));
}

/* ── UI 组件 ──────────────────────────────────────────────────────── */

/**
 * 设备统计是同步控制请求，不能在每帧都发 —— 那会阻塞 UI 线程。
 * 300ms 一次、150ms 超时，既够用又不会让界面卡顿。
 */
static bool refresh_stats(bool force)
{
    if (!g_dev) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && g_stats_valid && now - g_last_stats < std::chrono::milliseconds(300)) {
        return true;
    }
    g_last_stats = now;
    g_stats_valid = espgpu_get_stats(g_dev, &g_dev_stats, 150) == 0;
    return g_stats_valid;
}

static void draw_sidebar()
{
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 200, 255, 255));
    ImGui::TextUnformatted("ESP32-S3 GPU");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("panel v" ESPGPU_PANEL_VERSION);
    ImGui::Separator();

    struct Item {
        const char *icon;
        const char *name;
    };
    static const Item items[] = {
        {"[i]", "Overview"},
        {"[M]", "Display"},
        {"[P]", "Performance"},
        {"[V]", "Viewer"},
        {"[L]", "Log"},
    };

    for (int i = 0; i < IM_ARRAYSIZE(items); i++) {
        if (ImGui::Selectable(items[i].name, g_page == i, 0, ImVec2(0, 30))) {
            g_page = i;
        }
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", g_status);

    if (g_dev) {
        const espgpu_link_stats_t *ls = espgpu_link_stats(g_dev);
        if (ls) {
            ImGui::Text("%.1f fps", ls->fps);
            ImGui::TextDisabled("%.0f / %.0f KB/s", ls->in_kbps, ls->out_kbps);
        }
    }
}

static void draw_overview()
{
    ImGui::TextUnformatted("设备信息");
    ImGui::Separator();

    if (!g_dev) {
        ImGui::TextWrapped("未连接设备。");
        return;
    }

    if (!g_have_info) {
        g_have_info = espgpu_get_info(g_dev, &g_info, 500) == 0;
        if (!g_have_info) {
            ImGui::TextWrapped("无法读取设备信息。");
            return;
        }
    }

    if (ImGui::BeginTable("info", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        auto row = [](const char *k, const char *fmt, ...) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn();
            char buf[256];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            ImGui::TextUnformatted(buf);
        };

        row("芯片", "%s", g_info.chip_model);
        row("固件", "%u.%u.%u", g_info.fw_major, g_info.fw_minor, g_info.fw_patch);
        row("协议版本", "%u", g_info.proto_version);
        row("分辨率上限", "%u x %u", g_info.max_width, g_info.max_height);
        row("PSRAM", "%u MB（空闲 %u KB）", g_info.psram_mb, g_info.psram_free / 1024);
        row("内部 SRAM", "%u KB（空闲 %u KB）", g_info.sram_total / 1024,
            g_info.sram_free / 1024);
        row("Flash", "%u MB", g_info.flash_mb);
        row("纹理缓存", "%u KB", g_info.texture_cache_size / 1024);
        row("CPU", "%u MHz", g_info.cpu_freq_mhz);
        row("PSRAM 频率", "%u MHz", g_info.psram_freq_mhz);
        row("能力", "0x%04X", g_info.capabilities);
        row("编码", "0x%02X", g_info.codecs);
        row("设备路径", "%s", espgpu_device_path(g_dev));
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("设备端统计");
    ImGui::Separator();

    if (refresh_stats(false)) {
        ImGui::Text("运行时间      %u s", g_dev_stats.uptime_ms / 1000);
        ImGui::Text("已出帧        %u", g_dev_stats.frames_presented);
        ImGui::Text("丢帧          %u", g_dev_stats.frames_dropped);
        ImGui::Text("原始/编码     %u / %u 字节 (%.1fx)", g_dev_stats.frame_raw_bytes,
                    g_dev_stats.frame_enc_bytes,
                    g_dev_stats.frame_enc_bytes
                        ? static_cast<float>(g_dev_stats.frame_raw_bytes) /
                              static_cast<float>(g_dev_stats.frame_enc_bytes)
                        : 0.0f);
        ImGui::Text("生成/编码/发送 %.2f / %.2f / %.2f ms",
                    g_dev_stats.t_raster_us / 1000.0, g_dev_stats.t_encode_us / 1000.0,
                    g_dev_stats.t_usb_us / 1000.0);
        if (g_dev_stats.internal_temp_c10 != INT16_MIN) {
            ImGui::Text("芯片温度      %.1f °C", g_dev_stats.internal_temp_c10 / 10.0);
        } else {
            ImGui::Text("芯片温度      n/a");
        }
        ImGui::Text("空闲堆        %u KB / 最低 %u KB", g_dev_stats.free_heap / 1024,
                    g_dev_stats.min_free_heap / 1024);
    }
}

static void draw_display()
{
    ImGui::TextUnformatted("显示设置");
    ImGui::Separator();
    ImGui::TextWrapped("M1 阶段设备端还没有光栅器，这里控制的是链路测试模式。"
                       "M2 接入光栅器后，同一组参数会直接作用于渲染管线。");
    ImGui::Spacing();

    if (!g_dev) {
        ImGui::TextWrapped("未连接设备。");
        return;
    }

    static const int kRes[][2] = {{160, 120}, {320, 200}, {320, 240}, {400, 240}, {640, 480}};
    static const char *kResName[] = {"160x120", "320x200", "320x240", "400x240", "640x480"};
    int cur_res = -1;
    for (int i = 0; i < IM_ARRAYSIZE(kRes); i++) {
        if (g_mode_draft.width == kRes[i][0] && g_mode_draft.height == kRes[i][1]) {
            cur_res = i;
        }
    }

    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("分辨率", &cur_res, kResName, IM_ARRAYSIZE(kResName)) && cur_res >= 0) {
        g_mode_draft.width = static_cast<uint16_t>(kRes[cur_res][0]);
        g_mode_draft.height = static_cast<uint16_t>(kRes[cur_res][1]);
        g_mode_dirty = true;
    }

    static const char *kFmt[] = {"RGB565", "INDEX8（调色板）"};
    int fmt = (g_mode_draft.format == ESPGPU_FMT_INDEX8) ? 1 : 0;
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("像素格式", &fmt, kFmt, 2)) {
        g_mode_draft.format =
            static_cast<uint8_t>((fmt == 1) ? ESPGPU_FMT_INDEX8 : ESPGPU_FMT_RGB565);
        g_mode_dirty = true;
    }

    static const char *kCodec[] = {"RAW", "RLE", "DELTA_RLE"};
    int codec = static_cast<int>(g_mode_draft.codec);
    if (codec < 0 || codec > 2) {
        codec = 2;
    }
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("编码", &codec, kCodec, 3)) {
        g_mode_draft.codec = static_cast<uint8_t>(codec);
        g_mode_dirty = true;
    }

    int fps = g_mode_draft.max_fps;
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("目标帧率", &fps, 1, 60, "%d fps")) {
        g_mode_draft.max_fps = static_cast<uint8_t>(fps);
        g_mode_dirty = true;
    }

    static const char *kPattern[] = {"色条（平坦）", "界面（混合）", "Doom 近似", "噪声（最坏）",
                                     "上下混合"};
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("测试图案", &g_pattern_draft, kPattern, IM_ARRAYSIZE(kPattern))) {
        g_mode_dirty = true;
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!g_mode_dirty);
    if (ImGui::Button("应用", ImVec2(120, 0))) {
        const int st = espgpu_set_mode(g_dev, &g_mode_draft, static_cast<uint8_t>(g_pattern_draft),
                                       1000);
        if (st == 0) {
            push_ui_log(3, "mode applied: %ux%u fmt=%u codec=%u fps=%u", g_mode_draft.width,
                        g_mode_draft.height, g_mode_draft.format, g_mode_draft.codec,
                        g_mode_draft.max_fps);
            g_mode_dirty = false;
        } else {
            push_ui_log(1, "set_mode failed: status=%d", st);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("恢复默认", ImVec2(120, 0))) {
        g_mode_draft.width = 320;
        g_mode_draft.height = 200;
        g_mode_draft.format = static_cast<uint8_t>(ESPGPU_FMT_RGB565);
        g_mode_draft.codec = static_cast<uint8_t>(ESPGPU_CODEC_DELTA_RLE);
        g_mode_draft.max_fps = 24;
        g_pattern_draft = 2;
        g_mode_dirty = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("带宽预算");
    const float raw_kb = g_mode_draft.width * g_mode_draft.height *
                         (g_mode_draft.format == ESPGPU_FMT_INDEX8 ? 1.0f : 2.0f) / 1024.0f;
    const float need = raw_kb * g_mode_draft.max_fps / 1024.0f;
    ImGui::Text("单帧裸数据 %.1f KB，%d fps 需要 %.2f MB/s", raw_kb,
                g_mode_draft.max_fps, need);
    ImGui::TextDisabled("USB Full-Speed 实测上限约 0.85 MB/s");
    if (need > 0.85f) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           "裸传不可行，必须依赖 %.1fx 以上压缩（Delta+RLE 通常 8-15x）",
                           need / 0.85f);
    } else {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "裸传即可满足");
    }
}

static void draw_performance()
{
    ImGui::TextUnformatted("性能");
    ImGui::Separator();

    if (!g_dev) {
        ImGui::TextWrapped("未连接设备。");
        return;
    }

    const espgpu_link_stats_t *ls = espgpu_link_stats(g_dev);

    ImGui::Text("主机侧");
    ImGui::Text("帧率        %.1f fps", ls->fps);
    ImGui::Text("上行带宽    %.1f KB/s", ls->in_kbps);
    ImGui::Text("下行带宽    %.1f KB/s", ls->out_kbps);
    ImGui::Text("解码耗时    %.2f ms", ls->decode_ms);
    ImGui::Text("控制 RTT    %.2f ms", ls->rtt_ms);
    ImGui::Text("收到 / 丢弃 %u / %u", ls->frames_received, ls->frames_dropped);
    ImGui::Text("丢片 / 重同步 %u / %u", ls->fragments_lost, ls->resyncs);

    ImGui::Spacing();
    ImGui::Text("帧率");
    ImGui::PlotLines("##fps", g_hist_fps, kHistLen, g_hist_pos, nullptr, 0.0f, 60.0f,
                     ImVec2(0, 80));
    ImGui::Text("带宽（KB/s）");
    ImGui::PlotLines("##bw", g_hist_in, kHistLen, g_hist_pos, nullptr, 0.0f, 1200.0f,
                     ImVec2(0, 80));

    ImGui::Spacing();
    ImGui::Text("设备侧");
    if (refresh_stats(false)) {
        const float ratio = g_dev_stats.frame_enc_bytes
                                ? static_cast<float>(g_dev_stats.frame_raw_bytes) /
                                      static_cast<float>(g_dev_stats.frame_enc_bytes)
                                : 0.0f;
        ImGui::Text("压缩比      %.1fx  (%u -> %u 字节)", ratio, g_dev_stats.frame_raw_bytes,
                    g_dev_stats.frame_enc_bytes);
        ImGui::Text("生成        %.2f ms", g_dev_stats.t_raster_us / 1000.0);
        ImGui::Text("编码        %.2f ms", g_dev_stats.t_encode_us / 1000.0);
        ImGui::Text("USB 发送    %.2f ms", g_dev_stats.t_usb_us / 1000.0);
        ImGui::Text("空闲堆      %u KB", g_dev_stats.free_heap / 1024);
    }
}

static void draw_viewer()
{
    ImGui::TextUnformatted("帧查看器");
    ImGui::Separator();

    if (!g_dev) {
        ImGui::TextWrapped("未连接设备。");
        return;
    }

    if (g_frame_srv == nullptr) {
        ImGui::TextWrapped("等待第一帧…");
        return;
    }

    ImGui::Text("%d x %d   帧号 %u", g_tex_w, g_tex_h, espgpu_frame_id(g_dev));
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float aspect = static_cast<float>(g_tex_w) / static_cast<float>(g_tex_h);
    float w = avail.x;
    float h = w / aspect;
    if (h > avail.y) {
        h = avail.y;
        w = h * aspect;
    }

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - w) * 0.5f);
    ImGui::Image(reinterpret_cast<ImTextureID>(g_frame_srv), ImVec2(w, h),
                 ImVec2(0, 0), ImVec2(1, 1));
}

static void draw_log()
{
    if (ImGui::Button("清空")) {
        std::lock_guard<std::mutex> lk(g_log_mu);
        g_log.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &g_log_autoscroll);

    ImGui::Separator();
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);

    std::lock_guard<std::mutex> lk(g_log_mu);
    for (const auto &line : g_log) {
        ImVec4 color;
        switch (line.level) {
        case 1: color = ImVec4(1.0f, 0.45f, 0.45f, 1.0f); break;
        case 2: color = ImVec4(1.0f, 0.8f, 0.35f, 1.0f); break;
        case 3: color = ImVec4(0.75f, 0.85f, 1.0f, 1.0f); break;
        default: color = ImVec4(0.65f, 0.65f, 0.65f, 1.0f); break;
        }
        ImGui::TextColored(color, "%s", line.text.c_str());
    }

    if (g_log_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

/* ── WndProc ──────────────────────────────────────────────────────── */

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        return 1;
    }

    switch (msg) {
    case WM_SIZE:
        if (g_d3d != nullptr && wp != SIZE_MINIMIZED) {
            release_rtv();
            g_swap->ResizeBuffers(0, static_cast<UINT>(LOWORD(lp)),
                                  static_cast<UINT>(HIWORD(lp)), DXGI_FORMAT_UNKNOWN, 0);
            create_rtv();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ── main ─────────────────────────────────────────────────────────── */

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int)
{
    espgpu_set_log_callback(log_cb, nullptr);
    espgpu_set_log_level(3);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ESPGPU_PANEL";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ESP32-S3 GPU Control Panel",
                              WS_OVERLAPPEDWINDOW, 80, 80, 1180, 760, nullptr, nullptr, hinst,
                              nullptr);
    if (hwnd == nullptr) {
        return 1;
    }

    if (!init_d3d(hwnd)) {
        MessageBoxW(hwnd, L"Direct3D 11 初始化失败", L"ESP32-S3 GPU", MB_ICONERROR);
        cleanup_d3d();
        UnregisterClassW(wc.lpszClassName, hinst);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "espgpu-panel.ini";

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_d3d, g_ctx);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        if (g_swap_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_swap_occluded = false;

        /* 设备与帧 */
        try_connect();
        if (g_dev) {
            const int got = espgpu_poll(g_dev, 0);
            if (got > 0) {
                upload_frame();
            } else if (got < 0) {
                push_ui_log(1, "device poll failed");
                espgpu_close(g_dev);
                g_dev = nullptr;
                g_have_info = false;
                snprintf(g_status, sizeof(g_status), "设备错误，正在重连…");
            }
        }

        /* 历史曲线，200ms 采样一次 */
        const auto now = std::chrono::steady_clock::now();
        if (now - g_last_hist > std::chrono::milliseconds(200)) {
            g_last_hist = now;
            if (g_dev) {
                const espgpu_link_stats_t *ls = espgpu_link_stats(g_dev);
                g_hist_fps[g_hist_pos] = ls->fps;
                g_hist_in[g_hist_pos] = ls->in_kbps;
                g_hist_out[g_hist_pos] = ls->out_kbps;
            } else {
                g_hist_fps[g_hist_pos] = 0.0f;
                g_hist_in[g_hist_pos] = 0.0f;
                g_hist_out[g_hist_pos] = 0.0f;
            }
            g_hist_pos = (g_hist_pos + 1) % kHistLen;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

        ImGui::BeginChild("##sidebar", ImVec2(190, 0), ImGuiChildFlags_None);
        draw_sidebar();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_None);
        switch (g_page) {
        case 0: draw_overview(); break;
        case 1: draw_display(); break;
        case 2: draw_performance(); break;
        case 3: draw_viewer(); break;
        case 4: draw_log(); break;
        default: break;
        }
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();

        const float clear[4] = {0.08f, 0.09f, 0.11f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swap->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_dev) {
        espgpu_close(g_dev);
        g_dev = nullptr;
    }

    cleanup_d3d();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hinst);
    return 0;
}
