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

/* 过滤结果缓存：保存行的绝对序号（log_seq 计），只在过滤条件变化 / Clear 时全量重建，
 * 平时只追加新行、裁掉已被挤出队列的旧行 —— 5000 行 × 每帧子串搜索不再出现在热路径上。 */
struct FilterCache {
    std::vector<uint64_t> ids;
    uint64_t scanned = 0; /* 已检查到的序号（不含） */
    uint64_t clear_gen = ~0ull;
    int mask = -1;
    bool host = true, device = true;
    std::string search;
};
FilterCache g_fc;

bool line_passes(const LogLine &l, int mask)
{
    const int lv = l.level < 1 ? 1 : (l.level > 4 ? 4 : l.level);
    if (!((mask >> (lv - 1)) & 1)) return false;
    if (l.device ? !g_show_device : !g_show_host) return false;
    return contains_ci(l.text, g_search);
}

/** 必须持有 g_app.log_mu 调用 */
void update_filter(int mask)
{
    const uint64_t first = g_app.log_seq - g_app.log.size();
    if (g_fc.clear_gen != g_app.log_clear_gen || g_fc.mask != mask || g_fc.host != g_show_host ||
        g_fc.device != g_show_device || g_fc.search != g_search) {
        g_fc.ids.clear();
        g_fc.scanned = first;
        g_fc.clear_gen = g_app.log_clear_gen;
        g_fc.mask = mask;
        g_fc.host = g_show_host;
        g_fc.device = g_show_device;
        g_fc.search = g_search;
    }
    if (g_fc.scanned < first) g_fc.scanned = first;
    for (uint64_t q = g_fc.scanned; q < g_app.log_seq; q++) {
        if (line_passes(g_app.log[static_cast<size_t>(q - first)], mask)) g_fc.ids.push_back(q);
    }
    g_fc.scanned = g_app.log_seq;
    /* 裁掉已经出队的行（ids 升序） */
    size_t drop = 0;
    while (drop < g_fc.ids.size() && g_fc.ids[drop] < first) drop++;
    if (drop) g_fc.ids.erase(g_fc.ids.begin(), g_fc.ids.begin() + static_cast<ptrdiff_t>(drop));
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

    /* 持锁期间只做增量过滤和可见行渲染（ImGuiListClipper），锁占用是微秒级，
     * 不会卡住在 DLL 接收线程里回调 push_log 的一方 */
    std::lock_guard<std::mutex> lk(g_app.log_mu);
    update_filter(st.log_level_mask);
    const uint64_t first = g_app.log_seq - g_app.log.size();
    const std::vector<uint64_t> &idx = g_fc.ids;

    if (ui::button("Copy", ui::ButtonKind::Tonal, theme::icon::copy)) {
        std::string all;
        for (uint64_t q : idx) {
            const LogLine &l = g_app.log[static_cast<size_t>(q - first)];
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
            const LogLine &l = g_app.log[static_cast<size_t>(idx[static_cast<size_t>(r)] - first)];
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
        g_app.log_clear_gen++;
    }
}

} // namespace panel
