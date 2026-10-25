/*
 * clock.cpp — CPU 频率与温度
 */

#include "common.h"

namespace panel {

namespace {
const int kMhz[] = {80, 160, 240};
const char *const kMhzName[] = {"80 MHz", "160 MHz", "240 MHz"};
int g_pending = -1;
} // namespace

void page_clock()
{
    if (!require_device()) {
        return;
    }
    const theme::Scheme &s = theme::c();
    const Snapshot sn = snapshot();
    const History h = history_copy();
    const int cur = sn.valid && sn.s.cpu_freq_mhz ? sn.s.cpu_freq_mhz : sn.info.cpu_freq_mhz;
    char buf[48];

    const float w2 = ui::column_width(2);
    ui::card_begin("clk", w2);
    card_title("CPU clock");
    snprintf(buf, sizeof(buf), "%d", cur);
    ui::stat("Current", buf, "MHz");
    ImGui::Dummy(ImVec2(0, px(4)));
    if (require_cap(sn.info, ESPGPU_CAP_CLOCK, "clock control")) {
        int sel = 2;
        for (int k = 0; k < 3; k++) {
            if (kMhz[k] == cur) sel = k;
        }
        if (ui::segmented("mhz", kMhzName, 3, &sel) && kMhz[sel] != cur) {
            g_pending = sel;
            ui::open_dialog("Change CPU clock?");
        }
        ui::text_body_variant("Lower clocks reduce power and temperature, but the rasterizer "
                              "slows down proportionally. The setting is not persisted across "
                              "reboots.");
    }
    ui::card_end();

    ui::same_line_card();
    ui::card_begin("temp", w2);
    card_title("Temperature");
    if (sn.valid && sn.s.internal_temp_c10 != INT16_MIN) {
        const float t = sn.s.internal_temp_c10 / 10.0f;
        snprintf(buf, sizeof(buf), "%.1f", t);
        const ImVec4 col = t > 70 ? s.error : (t > 55 ? s.warning : s.success);
        ui::stat("Chip (internal sensor)", buf, "\xc2\xb0""C", &col,
                 t > 70 ? "Hot - consider lowering the clock" : "Within normal range");
    } else {
        ui::stat("Chip (internal sensor)", "n/a");
    }
    ui::card_end();

    ui::card_begin("tchart", 0);
    card_title("Temperature history", "Last 48 seconds");
    const ui::Series sr{h.temp_c, kHistLen, h.pos, s.tertiary, "\xc2\xb0""C"};
    ui::line_chart("tc", &sr, 1, 20.0f, 60.0f, ImVec2(0, px(200)), "\xc2\xb0""C");
    ui::card_end();

    char text[160] = "";
    if (g_pending >= 0) {
        snprintf(text, sizeof(text),
                 "Switch the CPU from %d MHz to %d MHz? The change applies immediately.", cur,
                 kMhz[g_pending]);
    }
    const int r = ui::dialog("Change CPU clock?", theme::icon::clock, "Change CPU clock?", text,
                             "Change");
    if (r == 1 && g_pending >= 0) {
        const int mhz = kMhz[g_pending];
        enqueue("Set clock", [mhz](espgpu_device_t *dev) { return espgpu_set_clock(dev, mhz, 1500); },
                "CPU clock changed");
        g_pending = -1;
    } else if (r == -1) {
        g_pending = -1;
    }
}

} // namespace panel
