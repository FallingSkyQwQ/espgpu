/*
 * debug.cpp — 调试视图（深度 / overdraw / tile / 线框）与诊断操作
 */

#include "common.h"

namespace panel {

namespace {

const char *const kViews[] = {"Normal", "Depth", "Overdraw", "Tiles", "Wireframe"};
const char *const kViewDesc[] = {
    "The rendered image, as presented.",
    "16-bit depth buffer as grayscale: near is bright, far is dark. Useful to check depth "
    "range, z-fighting and missing depth writes.",
    "Fragments shaded per pixel as a heat map. Bright areas are where the rasterizer spends "
    "the most time - look for large translucent overlays.",
    "Triangles binned into each 64x64 tile as a heat map, with tile borders. Shows how "
    "evenly the frame is split between the two cores.",
    "Triangle edges only. Reveals tessellation, degenerate triangles and clipping artifacts.",
};

int g_view = -1;
uint32_t g_view_gen = 0; /* g_view 属于哪一次连接（重启 / 重连后设备回到 Normal） */

void heat_legend(const char *lo, const char *hi)
{
    const theme::Scheme &s = theme::c();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = px(12);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    /* 与固件 raster3d.c 的 heat() 一致：黑 → 蓝 → 青 → 黄 → 红 */
    const ImU32 stops[] = {IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 255, 255),
                           IM_COL32(0, 255, 255, 255), IM_COL32(255, 255, 0, 255),
                           IM_COL32(255, 0, 0, 255)};
    const int n = 4;
    for (int k = 0; k < n; k++) {
        const float x0 = p.x + w * k / n, x1 = p.x + w * (k + 1) / n;
        dl->AddRectFilledMultiColor(ImVec2(x0, p.y), ImVec2(x1, p.y + h), stops[k], stops[k + 1],
                                    stops[k + 1], stops[k]);
    }
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), theme::u32(s.outline_variant), 0, 0, px(1));
    ui::text_label(lo);
    ImGui::SameLine(w - ImGui::CalcTextSize(hi).x);
    ui::text_label(hi);
}

} // namespace

void page_debug()
{
    if (!require_device()) {
        return;
    }
    const Snapshot sn = snapshot();
    if (g_view_gen != g_app.conn_gen.load()) {
        g_view_gen = g_app.conn_gen.load();
        g_view = -1;
    }
    /* 首次拿到有效统计时与设备同步（统计未到之前先显示 Normal，但不算已同步） */
    static bool s_synced = false;
    if (g_view < 0) s_synced = false;
    if (!s_synced && sn.valid) {
        g_view = sn.s.debug_view < ESPGPU_DEBUG_VIEW_COUNT ? sn.s.debug_view : 0;
        s_synced = true;
    }
    if (g_view < 0 || g_view >= ESPGPU_DEBUG_VIEW_COUNT) g_view = 0;

    if (require_cap(sn.info, ESPGPU_CAP_DEBUG_VIEW, "debug views")) {
        ui::card_begin("views", 0);
        card_title("Debug view",
                   "Replaces what the device sends back at PRESENT. Rendering itself is not "
                   "affected; switch back to Normal when done.");
        if (ui::segmented("dv", kViews, ESPGPU_DEBUG_VIEW_COUNT, &g_view)) {
            s_synced = true;
            const int v = g_view;
            enqueue("Set debug view", [v](espgpu_device_t *dev) {
                return espgpu_set_debug_view(dev, v, 800);
            }, v == 0 ? "Debug view off" : "Debug view applied");
        }
        ImGui::Dummy(ImVec2(0, px(2)));
        ui::text_body_variant("%s", kViewDesc[g_view]);
        if (g_view == ESPGPU_DEBUG_VIEW_OVERDRAW) {
            heat_legend("1 fragment", "8+ fragments");
        } else if (g_view == ESPGPU_DEBUG_VIEW_TILES) {
            heat_legend("empty tile", "busiest tile");
        }
        if (sn.valid && sn.s.debug_view != g_view) {
            ui::text_label("Device reports a different view - waiting for it to apply…");
        }
        ImGui::Dummy(ImVec2(0, px(4)));
        if (ui::button("Open viewer", ui::ButtonKind::Tonal, theme::icon::eye)) {
            g_app.page = PAGE_VIEWER;
        }
        ui::card_end();
    }

    const float w2 = ui::column_width(2);
    ui::card_begin("diag", w2);
    card_title("Diagnostics");
    if (ui::kv_begin("diagkv")) {
        ui::kv_row("Host errors (device side)", "%u", sn.s.host_errors);
        ui::kv_row("Sequence gaps", "%u", sn.s.seq_gaps);
        ui::kv_row("Commands executed", "%u", sn.s.cmd_count);
        ui::kv_row("Last host seq", "%u", sn.s.host_seq_seen);
        ui::kv_row("Bytes in / out", "%u / %u", sn.s.bytes_in, sn.s.bytes_out);
        ui::kv_end();
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    if (ui::button("Reset statistics", ui::ButtonKind::Tonal, theme::icon::refresh)) {
        enqueue("Reset statistics", [](espgpu_device_t *dev) {
            return espgpu_reset_stats(dev, 800);
        }, "Device statistics reset");
    }
    ImGui::SameLine(0, px(8));
    if (ui::button("Ping", ui::ButtonKind::Outlined, theme::icon::link)) {
        enqueue("Ping", [](espgpu_device_t *dev) { return espgpu_ping(dev, 800); }, "Pong");
    }
    ImGui::SameLine(0, px(8));
    if (ui::button("Request keyframe", ui::ButtonKind::Outlined, theme::icon::refresh)) {
        /* 画面出现残影 / 花屏时：让设备下一帧发非差分帧，把两边参考帧重新对齐 */
        enqueue("Request keyframe", [](espgpu_device_t *dev) {
            return espgpu_request_keyframe(dev, 800);
        }, "Keyframe requested");
    }
    ui::card_end();

    ui::same_line_card();
    ui::card_begin("reset", w2);
    card_title("Restart device",
               "Soft-resets the ESP32-S3. The USB link drops for a few seconds and the panel "
               "reconnects automatically.");
    if (ui::button("Restart", ui::ButtonKind::Danger, theme::icon::power)) {
        ui::open_dialog("Restart device?");
    }
    ui::card_end();

    if (ui::dialog("Restart device?", theme::icon::power, "Restart device?",
                   "Any program currently rendering on the GPU will lose its context.", "Restart",
                   true) == 1) {
        enqueue("Restart", [](espgpu_device_t *dev) { return espgpu_soft_reset(dev, 800); },
                "Device restarting…");
    }
}

} // namespace panel
