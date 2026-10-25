/*
 * overview.cpp — 设备总览
 */

#include "common.h"

namespace panel {

void page_overview()
{
    if (!require_device()) {
        return;
    }
    const theme::Scheme &s = theme::c();
    const Snapshot sn = snapshot();
    const espgpu_device_info_t &i = sn.info;

    /* ── 主卡片 ── */
    ui::card_begin("hero", 0, ui::CardKind::Hero);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::with_alpha(s.on_primary_container, 0.8f));
    ImGui::PushFont(theme::fonts().label);
    ImGui::TextUnformatted(g_app.emulator ? "SOFTWARE EMULATOR" : "GRAPHICS DEVICE");
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::PushFont(theme::fonts().headline);
    ImGui::Text("%s", sn.have_info && i.chip_model[0] ? i.chip_model : "ESP32-S3 GPU");
    ImGui::PopFont();
    if (sn.have_info) {
        ImGui::Text("Firmware %u.%u.%u  \xc2\xb7  protocol %u  \xc2\xb7  %u x %u max", i.fw_major,
                    i.fw_minor, i.fw_patch, i.proto_version, i.max_width, i.max_height);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme::with_alpha(s.on_primary_container, 0.75f));
    ImGui::TextWrapped("%s", g_app.device_path.c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, px(4)));

    int n = 0;
    const CapName *caps = cap_names(&n);
    const float row_w = ImGui::GetContentRegionAvail().x;
    float x = 0.0f;
    for (int k = 0; k < n; k++) {
        if (!(i.capabilities & caps[k].bit)) continue;
        const float cw = ImGui::CalcTextSize(caps[k].name).x + px(24);
        if (x > 0.0f && x + cw < row_w) {
            ImGui::SameLine(0, px(8));
        } else {
            x = 0.0f;
        }
        ui::chip(caps[k].name, s.surface_container_lowest, s.on_surface);
        x += cw + px(8);
    }
    ui::card_end();

    /* ── 数值磁贴 ── */
    ImGui::Dummy(ImVec2(0, px(4)));
    const float w4 = ui::column_width(4);
    char buf[64];

    ui::card_begin("t_up", w4, ui::CardKind::Filled);
    fmt_uptime(buf, sizeof(buf), sn.s.uptime_ms);
    ui::stat("Uptime", sn.valid ? buf : "-");
    ui::card_end();

    ui::same_line_card();
    ui::card_begin("t_temp", w4, ui::CardKind::Filled);
    if (sn.valid && sn.s.internal_temp_c10 != INT16_MIN) {
        const float t = sn.s.internal_temp_c10 / 10.0f;
        snprintf(buf, sizeof(buf), "%.1f", t);
        const ImVec4 col = t > 70 ? s.error : (t > 55 ? s.warning : s.on_surface);
        ui::stat("Chip temperature", buf, "\xc2\xb0""C", &col);
    } else {
        ui::stat("Chip temperature", "n/a");
    }
    ui::card_end();

    ui::same_line_card();
    ui::card_begin("t_frames", w4, ui::CardKind::Filled);
    snprintf(buf, sizeof(buf), "%u", sn.s.frames_presented);
    ui::stat("Frames presented", sn.valid ? buf : "-");
    ui::card_end();

    ui::same_line_card();
    ui::card_begin("t_drop", w4, ui::CardKind::Filled);
    snprintf(buf, sizeof(buf), "%u", sn.s.frames_dropped);
    ui::stat("Frames dropped", sn.valid ? buf : "-", nullptr,
             sn.s.frames_dropped ? &s.warning : nullptr);
    ui::card_end();

    /* ── 硬件 / 链路 ── */
    const float w2 = ui::column_width(2);
    ui::card_begin("hw", w2);
    card_title("Hardware");
    if (ui::kv_begin("hwkv")) {
        char a[32], b[32];
        ui::kv_row("Chip", "%s", i.chip_model);
        ui::kv_row("CPU", "%u MHz", sn.valid && sn.s.cpu_freq_mhz ? sn.s.cpu_freq_mhz : i.cpu_freq_mhz);
        fmt_bytes(a, sizeof(a), i.psram_free);
        ui::kv_row("PSRAM", "%u MB @ %u MHz  (%s free)", i.psram_mb, i.psram_freq_mhz, a);
        fmt_bytes(a, sizeof(a), i.sram_total);
        fmt_bytes(b, sizeof(b), i.sram_free);
        ui::kv_row("Internal SRAM", "%s  (%s free)", a, b);
        ui::kv_row("Flash", "%u MB", i.flash_mb);
        fmt_bytes(a, sizeof(a), i.texture_cache_size);
        ui::kv_row("Texture cache", "%s, max %u px", a, i.max_texture_dim);
        ui::kv_row("Texture units", "%u", i.max_texture_units);
        ui::kv_row("Vertex attributes", "%u", i.max_vertex_attribs);
        ui::kv_end();
    }
    ui::card_end();

    ui::same_line_card();
    ui::card_begin("link", w2);
    card_title("Link");
    if (ui::kv_begin("linkkv")) {
        ui::kv_row("Transport", "%s", g_app.emulator ? "In-process emulator" : "USB Full-Speed (WinUSB)");
        ui::kv_row("Frame rate", "%.1f fps", sn.link.fps);
        ui::kv_row("Uplink", "%.1f KB/s", sn.link.in_kbps);
        ui::kv_row("Downlink", "%.1f KB/s", sn.link.out_kbps);
        ui::kv_row("Control RTT", "%.2f ms", sn.link.rtt_ms);
        ui::kv_row("Codecs", "%s%s%s%s", (i.codecs & ESPGPU_CAP_CODEC_RAW) ? "RAW " : "",
                   (i.codecs & ESPGPU_CAP_CODEC_RLE) ? "RLE " : "",
                   (i.codecs & ESPGPU_CAP_CODEC_DELTA_RLE) ? "DELTA_RLE " : "",
                   (i.codecs & ESPGPU_CAP_CODEC_TILE_DELTA) ? "TILE_DELTA" : "");
        ui::kv_row("Received / dropped", "%u / %u", sn.link.frames_received, sn.link.frames_dropped);
        ui::kv_row("Fragments lost / resyncs", "%u / %u", sn.link.fragments_lost, sn.link.resyncs);
        ui::kv_end();
    }
    ui::card_end();
}

} // namespace panel
