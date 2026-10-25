/*
 * memory.cpp — PSRAM / SRAM / 纹理缓存 / 设备缓冲
 */

#include "common.h"

namespace panel {

void page_memory()
{
    if (!require_device()) {
        return;
    }
    const theme::Scheme &s = theme::c();
    const Snapshot sn = snapshot();
    const espgpu_device_info_t &i = sn.info;
    const double MB = 1024.0 * 1024.0;
    const double KB = 1024.0;

    ui::card_begin("mem", 0);
    card_title("Memory", "PSRAM holds frame buffers, textures and command buffers; internal SRAM "
                         "holds tile buffers and hot code.");
    if (sn.valid && i.psram_total) {
        const double used = static_cast<double>(i.psram_total) - sn.s.free_psram;
        ui::usage_bar("PSRAM", used / MB, i.psram_total / MB, "MB");
    }
    if (sn.valid && i.sram_total) {
        /* free_heap 包含 PSRAM，内部 SRAM 用量只能从握手时的 sram_free 估算 */
        const double used = static_cast<double>(i.sram_total) - i.sram_free;
        ui::usage_bar("Internal SRAM (at connect)", used / KB, i.sram_total / KB, "KB", &s.tertiary);
    }
    if (sn.valid) {
        char a[32], b[32];
        fmt_bytes(a, sizeof(a), sn.s.free_heap);
        fmt_bytes(b, sizeof(b), sn.s.min_free_heap);
        ui::text_body_variant("Free heap %s, lowest since boot %s", a, b);
    }
    ui::card_end();

    if (!require_cap(i, ESPGPU_CAP_TEX_LRU, "the texture cache")) {
        return;
    }

    ui::card_begin("tex", 0);
    card_title("Texture cache", "Least-recently-used textures are evicted when the cache is "
                                "full; the driver re-uploads them on next use.");
    if (i.texture_cache_size) {
        ui::usage_bar("Cache", sn.s.tex_bytes / KB, i.texture_cache_size / KB, "KB", &s.secondary);
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    const float w4 = ui::column_width(4);
    char buf[48];
    uint32_t seen = 0;
    {
        std::lock_guard<std::mutex> lk(g_app.stats_mu);
        seen = g_app.evictions_seen;
    }
    struct Tile { const char *label; uint32_t v; const char *sub; };
    const Tile tiles[] = {
        {"Resident textures", sn.s.tex_resident, nullptr},
        {"Evictions", sn.s.tex_evictions, nullptr},
        {"Cache misses", sn.s.tex_misses, "draws with a missing texture"},
        {"Eviction events", seen, "seen by this panel"},
    };
    for (int k = 0; k < 4; k++) {
        if (k) ui::same_line_card();
        char id[16];
        snprintf(id, sizeof(id), "mt%d", k);
        ui::card_begin(id, w4, ui::CardKind::Filled);
        snprintf(buf, sizeof(buf), "%u", tiles[k].v);
        const ImVec4 *acc = (k == 2 && tiles[k].v) ? &s.warning : nullptr;
        ui::stat(tiles[k].label, buf, nullptr, acc, tiles[k].sub);
        ui::card_end();
    }
    ui::card_end();

    ui::card_begin("bufs", 0);
    card_title("Other allocations");
    if (ui::kv_begin("bufkv")) {
        char a[32];
        fmt_bytes(a, sizeof(a), sn.s.buffers_bytes);
        ui::kv_row("Device buffers (display lists)", "%s", a);
        fmt_bytes(a, sizeof(a), sn.s.bin_bytes);
        ui::kv_row("Tile bins (last frame)", "%s", a);
        fmt_bytes(a, sizeof(a), sn.s.frame_raw_bytes);
        ui::kv_row("Frame buffer", "%s", a);
        ui::kv_end();
    }
    ui::card_end();
}

} // namespace panel
