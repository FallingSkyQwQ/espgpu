/*
 * display.cpp — 显示模式（分辨率 / 格式 / 编码 / 帧率 / 测试图案）
 */

#include "common.h"

namespace panel {

namespace {

const int kRes[][2] = {{160, 120}, {320, 200}, {320, 240}, {400, 240}, {640, 480}};
const char *const kResName[] = {"160x120", "320x200", "320x240", "400x240", "640x480"};
const char *const kFmt[] = {"RGB565", "INDEX8"};
const char *const kCodec[] = {"RAW", "RLE", "DELTA_RLE"};
/* 命令模式的编码多两个：有损块编码与自适应（见 espgpu_proto.h 的 ESPGPU_CODEC_BLOCK / AUTO） */
const char *const kRenderCodec[] = {"RAW", "RLE", "DELTA", "BLOCK", "AUTO"};
const uint8_t kRenderCodecValue[] = {ESPGPU_CODEC_RAW, ESPGPU_CODEC_RLE, ESPGPU_CODEC_DELTA_RLE,
                                     ESPGPU_CODEC_BLOCK, ESPGPU_CODEC_AUTO};
const char *const kPattern[] = {"Bars", "UI", "Doom-like", "Noise", "Mixed"};

void defaults(ModeDraft &d)
{
    d.mode.width = 320;
    d.mode.height = 200;
    d.mode.format = ESPGPU_FMT_RGB565;
    d.mode.codec = ESPGPU_CODEC_DELTA_RLE;
    d.mode.max_fps = 24;
    d.mode.vsync = 1;
    d.pattern = ESPGPU_PATTERN_DOOMISH;
}

} // namespace

void page_display()
{
    if (!require_device()) {
        return;
    }
    const theme::Scheme &s = theme::c();
    ModeDraft &d = g_app.draft;
    if (d.mode.width == 0) {
        defaults(d);
    }

    /* ── 显示模式 ── */
    ui::card_begin("mode", 0);
    card_title("Display mode",
               "Resolution, pixel format and frame codec of the stream sent back over USB. "
               "While no program is drawing, the device streams the selected test pattern.");

    ImGui::Dummy(ImVec2(0, px(4)));
    ui::text_label("Resolution");
    int res = -1;
    for (int k = 0; k < 5; k++) {
        if (d.mode.width == kRes[k][0] && d.mode.height == kRes[k][1]) res = k;
    }
    if (ui::segmented("res", kResName, 5, &res) && res >= 0) {
        d.mode.width = static_cast<uint16_t>(kRes[res][0]);
        d.mode.height = static_cast<uint16_t>(kRes[res][1]);
        d.dirty = true;
    }

    const float half = ui::column_width(2);
    ImGui::BeginGroup();
    ui::text_label("Pixel format");
    int fmt = d.mode.format == ESPGPU_FMT_INDEX8 ? 1 : 0;
    if (ui::segmented("fmt", kFmt, 2, &fmt, half)) {
        d.mode.format = static_cast<uint8_t>(fmt ? ESPGPU_FMT_INDEX8 : ESPGPU_FMT_RGB565);
        d.dirty = true;
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, px(16));
    ImGui::BeginGroup();
    ui::text_label("Frame codec");
    int codec = d.mode.codec <= 2 ? d.mode.codec : 2;
    if (ui::segmented("codec", kCodec, 3, &codec, half)) {
        d.mode.codec = static_cast<uint8_t>(codec);
        d.dirty = true;
    }
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, px(2)));
    int fps = d.mode.max_fps;
    if (ui::slider_int("Target frame rate", &fps, 1, 60, "%d fps")) {
        d.mode.max_fps = static_cast<uint8_t>(fps);
        d.dirty = true;
    }

    ui::text_label("Test pattern");
    if (ui::segmented("pattern", kPattern, 5, &d.pattern)) {
        d.dirty = true;
    }

    ImGui::Dummy(ImVec2(0, px(6)));
    if (ui::button("Apply", ui::ButtonKind::Filled, theme::icon::check, 0, d.dirty)) {
        const espgpu_mode_t m = d.mode;
        const uint8_t pat = static_cast<uint8_t>(d.pattern);
        enqueue("Set mode", [m, pat](espgpu_device_t *dev) {
            return espgpu_set_mode(dev, &m, pat, 1500);
        }, "Display mode applied");
        d.dirty = false;
    }
    ImGui::SameLine(0, px(8));
    if (ui::button("Restore defaults", ui::ButtonKind::Outlined, theme::icon::refresh)) {
        defaults(d);
        d.dirty = true;
    }
    ui::card_end();

    /* ── 渲染编码 ── */
    const float w2 = ui::column_width(2);
    ui::card_begin("rcodec", w2);
    card_title("Rendering codec",
               "Codec for frames produced by draw commands. DELTA is lossless and best for "
               "static scenes. BLOCK is lossy (BC1, fixed 4:1, unchanged blocks skipped) and keeps "
               "full-screen motion within the USB budget. AUTO uses lossless frames while they "
               "fit and switches to BLOCK during motion, then refreshes losslessly.");
    int rc_idx = 2;
    for (int k = 0; k < IM_ARRAYSIZE(kRenderCodecValue); k++) {
        if (kRenderCodecValue[k] == d.render_codec) {
            rc_idx = k;
        }
    }
    const bool has_new = (snapshot().info.codecs & ESPGPU_CAP_CODEC_AUTO) != 0;
    if (ui::segmented("rc", kRenderCodec, has_new ? 5 : 3, &rc_idx)) {
        d.render_codec = kRenderCodecValue[rc_idx];
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    if (ui::button("Apply codec", ui::ButtonKind::Tonal)) {
        const int rc = d.render_codec;
        enqueue("Set codec", [rc](espgpu_device_t *dev) { return espgpu_set_codec(dev, rc, 800); },
                "Rendering codec applied");
    }
    ui::card_end();

    /* ── 带宽预算 ── */
    ui::same_line_card();
    ui::card_begin("budget", w2);
    card_title("Bandwidth budget");
    const float bpp = d.mode.format == ESPGPU_FMT_INDEX8 ? 1.0f : 2.0f;
    const float raw_kb = d.mode.width * d.mode.height * bpp / 1024.0f;
    const float need_mb = raw_kb * d.mode.max_fps / 1024.0f;
    const float limit_mb = 0.85f;
    ImGui::Text("Raw frame %.1f KB, %u fps needs %.2f MB/s uncompressed", raw_kb, d.mode.max_fps,
                need_mb);
    const ImVec4 col = need_mb > limit_mb ? s.warning : s.success;
    ui::progress(std::min(1.0f, need_mb / (limit_mb * 4.0f)), 0, &col);
    if (need_mb > limit_mb) {
        ui::text_body_variant("Raw transfer exceeds the ~%.2f MB/s USB Full-Speed limit: needs at "
                              "least %.1fx compression (DELTA_RLE usually reaches 8-15x).",
                              limit_mb, need_mb / limit_mb);
    } else {
        ui::text_body_variant("Fits the ~%.2f MB/s USB Full-Speed limit without compression.",
                              limit_mb);
    }
    const Snapshot sn = snapshot();
    ImGui::Dummy(ImVec2(0, px(2)));
    ImGui::Text("Measured uplink  %.0f KB/s", sn.link.in_kbps);
    const ImVec4 mcol = s.primary;
    ui::progress(std::min(1.0f, sn.link.in_kbps / (limit_mb * 1024.0f)), 0, &mcol);
    ui::card_end();
}

} // namespace panel
