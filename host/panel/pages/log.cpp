/*
 * log.cpp — 主机与设备日志
 */

#include "common.h"

#include <cctype>
#include <string>
#include <vector>

namespace panel {

namespace {

char g_search[128] = "";
bool g_autoscroll = true;
bool g_show_host = true;
bool g_show_device = true;

bool contains_ci(const std::string &hay, const char *needle)
{
    if (!needle[0]) return true;
    const size_t n = strlen(needle);
    if (hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); i++) {
        size_t k = 0;
        while (k < n && tolower(static_cast<unsigned char>(hay[i + k])) ==
                            tolower(static_cast<unsigned char>(needle[k]))) {
            k++;
        }
        if (k == n) return true;
    }
    return false;
}

} // namespace

void page_log()
{
    const theme::Scheme &s = theme::c();
    Settings &st = g_app.settings;

    /* ── 工具栏 ── */
    ui::text_field("search", "Search log", g_search, sizeof(g_search), theme::icon::search,
                   px(320));
    ImGui::SameLine(0, px(16));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(6));

    static const char *const kLevels[] = {"Errors", "Warnings", "Info", "Debug"};
    for (int k = 0; k < 4; k++) {
        bool on = (st.log_level_mask >> k) & 1;
        if (k) ImGui::SameLine(0, px(8));
        if (ui::filter_chip(kLevels[k], &on)) {
            st.log_level_mask = on ? (st.log_level_mask | (1 << k)) : (st.log_level_mask & ~(1 << k));
            save_settings();
        }
    }
    ImGui::SameLine(0, px(16));
    ui::filter_chip("Host", &g_show_host);
    ImGui::SameLine(0, px(8));
    ui::filter_chip("Device", &g_show_device);

    const Snapshot sn = snapshot();
    const bool can_sub = connected() && (sn.info.capabilities & ESPGPU_CAP_LOG);
    bool dev_log = st.device_log;
    if (ui::toggle("Stream device log", &dev_log)) {
        st.device_log = dev_log;
        save_settings();
        if (can_sub) {
            const int lvl = dev_log ? 3 : 0;
            enqueue("Device log", [lvl](espgpu_device_t *dev) {
                return espgpu_log_subscribe(dev, lvl, 800);
            }, dev_log ? "Device log streaming on" : "Device log streaming off");
        } else if (dev_log) {
            ui::snackbar(ui::SnackKind::Info,
                         "Device log will start when a device with log support connects");
        }
    }
    ImGui::SameLine(0, px(24));
    ui::toggle("Auto-scroll", &g_autoscroll);
    ImGui::SameLine(0, px(24));

    /* 收集过滤后的行（持锁期间只做索引，渲染也在锁内，避免拷贝 5000 个字符串） */
    std::lock_guard<std::mutex> lk(g_app.log_mu);
    std::vector<int> idx;
    idx.reserve(g_app.log.size());
    for (int k = 0; k < static_cast<int>(g_app.log.size()); k++) {
        const LogLine &l = g_app.log[static_cast<size_t>(k)];
        const int lv = l.level < 1 ? 1 : (l.level > 4 ? 4 : l.level);
        if (!((st.log_level_mask >> (lv - 1)) & 1)) continue;
        if (l.device ? !g_show_device : !g_show_host) continue;
        if (!contains_ci(l.text, g_search)) continue;
        idx.push_back(k);
    }

    if (ui::button("Copy", ui::ButtonKind::Tonal, theme::icon::copy)) {
        std::string all;
        for (int k : idx) {
            const LogLine &l = g_app.log[static_cast<size_t>(k)];
            char head[48];
            snprintf(head, sizeof(head), "[%9.3f] %s ", l.time, l.device ? "DEV " : "HOST");
            all += head;
            all += l.text;
            all += '\n';
        }
        ImGui::SetClipboardText(all.c_str());
        ui::snackbar(ui::SnackKind::Success, "Copied %d lines", static_cast<int>(idx.size()));
    }
    ImGui::SameLine(0, px(8));
    bool clear = ui::button("Clear", ui::ButtonKind::Text, theme::icon::del);

    ImGui::Dummy(ImVec2(0, px(4)));
    {
        char cnt[48];
        if (idx.empty()) {
            snprintf(cnt, sizeof(cnt), "No matching lines");
        } else {
            snprintf(cnt, sizeof(cnt), "%d of %d lines", static_cast<int>(idx.size()),
                     static_cast<int>(g_app.log.size()));
        }
        ui::text_label(cnt);
    }

    /* ── 列表 ── */
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float h = std::max(avail.y - px(4), px(200));
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + avail.x, p.y + h),
                                              theme::u32(s.surface_container_lowest), px(16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(12)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(2)));
    ImGui::BeginChild("##loglist", ImVec2(avail.x, h), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(idx.size()));
    ImFont *mono = theme::fonts().label;
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            const LogLine &l = g_app.log[static_cast<size_t>(idx[static_cast<size_t>(r)])];
            ImVec4 col;
            const char *tag;
            switch (l.level) {
            case 1: col = s.error; tag = "ERR"; break;
            case 2: col = s.warning; tag = "WRN"; break;
            case 3: col = s.on_surface; tag = "INF"; break;
            default: col = s.on_surface_variant; tag = "DBG"; break;
            }
            ImGui::PushFont(mono);
            ImGui::TextColored(s.on_surface_variant, "%9.3f", l.time);
            ImGui::SameLine();
            ImGui::TextColored(l.device ? s.tertiary : s.primary, "%s", l.device ? "DEV " : "HOST");
            ImGui::SameLine();
            ImGui::TextColored(col, "%s", tag);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (g_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - px(20)) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    if (clear) {
        g_app.log.clear();
    }
}

} // namespace panel
