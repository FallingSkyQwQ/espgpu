/*
 * performance.cpp — 帧率、带宽、帧耗时构成、几何计数
 */

#include "common.h"

namespace panel {

void page_performance()
{
    if (!require_device()) {
        return;
    }
    const theme::Scheme &s = theme::c();
    const Snapshot sn = snapshot();
    const History h = history_copy();
    char buf[64];

    /* ── 磁贴 ── */
    const float w5 = ui::column_width(5);
    struct Tile {
        const char *label;
        float value;
        const char *fmt;
        const char *unit;
    };
    const Tile tiles[] = {
        {"Frame rate", sn.link.fps, "%.1f", "fps"},
        {"Uplink", sn.link.in_kbps, "%.0f", "KB/s"},
        {"Downlink", sn.link.out_kbps, "%.0f", "KB/s"},
        {"Control RTT", sn.link.rtt_ms, "%.2f", "ms"},
        {"Host decode", sn.link.decode_ms, "%.2f", "ms"},
    };
    for (int k = 0; k < 5; k++) {
        if (k) ui::same_line_card();
        char id[16];
        snprintf(id, sizeof(id), "pt%d", k);
        ui::card_begin(id, w5, ui::CardKind::Filled);
        snprintf(buf, sizeof(buf), tiles[k].fmt, tiles[k].value);
        ui::stat(tiles[k].label, buf, tiles[k].unit);
        ui::card_end();
    }

    /* ── 曲线 ── */
    const float w2 = ui::column_width(2);
    ui::card_begin("c_fps", w2);
    card_title("Frame rate", "Last 48 seconds, host side");
    {
        const ui::Series sr{h.fps, kHistLen, h.pos, s.primary, "fps"};
        ui::line_chart("fps", &sr, 1, 0.0f, 30.0f, ImVec2(0, px(170)), "fps");
    }
    ui::card_end();
    ui::same_line_card();
    ui::card_begin("c_bw", w2);
    card_title("Bandwidth", "Uplink = frames, downlink = commands");
    {
        const ui::Series sr[2] = {{h.in_kbps, kHistLen, h.pos, s.primary, "Uplink"},
                                  {h.out_kbps, kHistLen, h.pos, s.tertiary, "Downlink"}};
        ui::line_chart("bw", sr, 2, 0.0f, 400.0f, ImVec2(0, px(170)), "KB/s");
    }
    ui::card_end();

    /* ── 帧耗时 ── */
    ui::card_begin("ftime", 0);
    card_title("Device frame time",
               "Measured on the device for the last frame. The dashed line is the 24 fps budget "
               "(41.7 ms).");
    if (sn.valid) {
        const ui::Segment segs[] = {
            {sn.s.t_raster_us / 1000.0f, s.primary, "Raster"},
            {sn.s.t_tnl_us / 1000.0f, s.tertiary, "T&L + binning"},
            {sn.s.t_encode_us / 1000.0f, s.secondary, "Encode"},
            {sn.s.t_usb_us / 1000.0f, s.warning, "USB"},
        };
        ui::stacked_bar("fb", segs, 4, 1000.0f / 24.0f, 0, "ms");
        ImGui::Dummy(ImVec2(0, px(6)));
        const ui::Series sr{h.frame_ms, kHistLen, h.pos, s.secondary, "total"};
        ui::line_chart("ft", &sr, 1, 0.0f, 50.0f, ImVec2(0, px(110)), "ms");
    } else {
        ui::text_body_variant("Waiting for device statistics…");
    }
    ui::card_end();

    /* ── 计数 ── */
    ui::card_begin("geo", w2);
    card_title("Geometry (last frame)");
    if (ui::kv_begin("geokv")) {
        ui::kv_row("Draw calls", "%u", sn.s.draw_calls);
        ui::kv_row("Primitives", "%u", sn.s.primitives);
        ui::kv_row("Vertices", "%u", sn.s.vertices);
        ui::kv_row("Triangles rasterized", "%u", sn.s.triangles);
        ui::kv_row("Triangles culled", "%u", sn.s.culled);
        ui::kv_row("Pixels filled", "%u", sn.s.fill_pixels);
        ui::kv_row("Dirty tiles", "%u", sn.s.tiles_dirty);
        ui::kv_end();
    }
    ui::card_end();

    ui::same_line_card();
    ui::card_begin("comp", w2);
    card_title("Compression & link");
    if (ui::kv_begin("compkv")) {
        const float ratio = sn.s.frame_enc_bytes
                                ? static_cast<float>(sn.s.frame_raw_bytes) / sn.s.frame_enc_bytes
                                : 0.0f;
        ui::kv_row("Frame codec ratio", "%.1fx  (%u -> %u B)", ratio, sn.s.frame_raw_bytes,
                   sn.s.frame_enc_bytes);
        if (sn.s.lz_in_bytes) {
            ui::kv_row("Downlink LZ ratio", "%.2fx",
                       static_cast<float>(sn.s.lz_out_bytes) / sn.s.lz_in_bytes);
        } else {
            ui::kv_row("Downlink LZ ratio", "not used");
        }
        ui::kv_row("Frames received", "%u", sn.link.frames_received);
        ui::kv_row("Frames dropped", "%u host / %u device", sn.link.frames_dropped,
                   sn.s.frames_dropped);
        ui::kv_row("Fragments lost", "%u", sn.link.fragments_lost);
        ui::kv_row("Resyncs", "%u", sn.link.resyncs);
        ui::kv_row("CRC errors", "%u host / %u device", sn.link.crc_errors, sn.s.crc_errors);
        ui::kv_end();
    }
    ui::card_end();
}

} // namespace panel
