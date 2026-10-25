/*
 * pages/common.h — 各页面共用的小工具
 */

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "../app.h"
#include "../theme.h"
#include "../widgets.h"

namespace panel {

using theme::px;

/** 能力位名称表 */
struct CapName {
    uint16_t bit;
    const char *name;
};

inline const CapName *cap_names(int *count)
{
    static const CapName kCaps[] = {
        {ESPGPU_CAP_INDEX8, "INDEX8"},        {ESPGPU_CAP_RGB565, "RGB565"},
        {ESPGPU_CAP_DEPTH, "Depth"},          {ESPGPU_CAP_FBO, "FBO"},
        {ESPGPU_CAP_FRAGMENT_VM, "Fragment VM"}, {ESPGPU_CAP_QUADS, "Quads"},
        {ESPGPU_CAP_LIGHTING, "Lighting"},    {ESPGPU_CAP_OTA, "OTA"},
        {ESPGPU_CAP_TNL, "T&L"},              {ESPGPU_CAP_TEX_LRU, "Texture LRU"},
        {ESPGPU_CAP_PALETTED_TEX, "Paletted textures"}, {ESPGPU_CAP_BUFFERS, "Device buffers"},
        {ESPGPU_CAP_CLOCK, "Clock control"},  {ESPGPU_CAP_DEBUG_VIEW, "Debug views"},
        {ESPGPU_CAP_LZ, "LZ downlink"},       {ESPGPU_CAP_LOG, "Device log"},
    };
    *count = static_cast<int>(sizeof(kCaps) / sizeof(kCaps[0]));
    return kCaps;
}

inline void fmt_uptime(char *buf, size_t cap, uint32_t ms)
{
    const uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(buf, cap, "%uh %02um", s / 3600, (s / 60) % 60);
    } else if (s >= 60) {
        snprintf(buf, cap, "%um %02us", s / 60, s % 60);
    } else {
        snprintf(buf, cap, "%us", s);
    }
}

inline void fmt_bytes(char *buf, size_t cap, double bytes)
{
    if (bytes >= 1024.0 * 1024.0) {
        snprintf(buf, cap, "%.2f MB", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024.0) {
        snprintf(buf, cap, "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, cap, "%.0f B", bytes);
    }
}

/** UTF-16 → UTF-8（任意长度；失败返回空串） */
inline std::string narrow(const std::wstring &w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

/** 历史曲线的线程安全拷贝 */
inline History history_copy()
{
    std::lock_guard<std::mutex> lk(g_app.stats_mu);
    return g_app.hist;
}

/** 卡片标题行：标题 + 可选副标题 */
inline void card_title(const char *title, const char *sub = nullptr)
{
    ImGui::PushFont(theme::fonts().title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (sub) {
        ui::text_body_variant("%s", sub);
    }
    ImGui::Dummy(ImVec2(0, px(2)));
}

/** 设备不支持某能力时的提示条 */
inline bool require_cap(const espgpu_device_info_t &info, uint16_t cap, const char *feature)
{
    if (info.capabilities & cap) {
        return true;
    }
    const theme::Scheme &s = theme::c();
    ui::card_begin(feature, 0, ui::CardKind::Filled);
    ui::text_colored(s.warning, "%s", theme::fonts().have_icons ? theme::icon::warning : "!");
    ImGui::SameLine();
    ImGui::TextWrapped("This firmware does not report %s support (capability bit 0x%04X). "
                       "Update the firmware on the Firmware page.",
                       feature, cap);
    ui::card_end();
    return false;
}

} // namespace panel
