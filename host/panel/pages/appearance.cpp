/*
 * appearance.cpp — 主题与动效设置，附带当前配色预览
 */

#include "common.h"

namespace panel {

namespace {

void swatch(const char *name, const ImVec4 &bg, const ImVec4 &fg, float w)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = px(64);
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::u32(bg), px(12));
    ImFont *f = theme::fonts().label;
    dl->AddText(f, f->FontSize, ImVec2(p.x + px(12), p.y + h - f->FontSize - px(10)),
                theme::u32(fg), name);
}

} // namespace

void page_appearance()
{
    const theme::Scheme &s = theme::c();
    Settings &st = g_app.settings;

    ui::card_begin("theme", 0);
    card_title("Theme", "Material 3 tonal color scheme. System follows the Windows app theme.");
    static const char *const kModes[] = {"Dark", "Light", "System"};
    if (ui::segmented("mode", kModes, 3, &st.theme_mode, px(360))) {
        apply_theme_settings();
        save_settings();
    }
    ImGui::Dummy(ImVec2(0, px(6)));
    if (ui::toggle("Reduce motion", &st.reduce_motion, px(360))) {
        apply_theme_settings();
        save_settings();
    }
    ui::text_body_variant("Turns off page fades and component transitions.");
    ui::card_end();

    ui::card_begin("roles", 0);
    card_title("Color roles", "The current scheme, generated from the seed tone palettes.");
    struct Role { const char *name; ImVec4 bg, fg; };
    const Role roles[] = {
        {"Primary", s.primary, s.on_primary},
        {"Primary container", s.primary_container, s.on_primary_container},
        {"Secondary", s.secondary, s.on_secondary},
        {"Secondary container", s.secondary_container, s.on_secondary_container},
        {"Tertiary", s.tertiary, s.on_tertiary},
        {"Tertiary container", s.tertiary_container, s.on_tertiary_container},
        {"Error", s.error, s.on_error},
        {"Error container", s.error_container, s.on_error_container},
        {"Surface", s.surface, s.on_surface},
        {"Container low", s.surface_container_low, s.on_surface},
        {"Container", s.surface_container, s.on_surface},
        {"Container highest", s.surface_container_highest, s.on_surface},
    };
    const int cols = 4;
    const float w = ui::column_width(cols);
    for (int k = 0; k < 12; k++) {
        if (k % cols) ImGui::SameLine(0, px(16));
        swatch(roles[k].name, roles[k].bg, roles[k].fg, w);
    }
    ui::card_end();

    ui::card_begin("about", 0, ui::CardKind::Outlined);
    card_title("About");
    ui::text_body_variant("ESP32-S3 GPU control panel. Win32 + Direct3D 11 + Dear ImGui %s.",
                          IMGUI_VERSION);
    ui::text_body_variant("Fonts: Segoe UI Variable, Microsoft YaHei UI (CJK), Segoe Fluent "
                          "Icons - loaded from %%WINDIR%%\\Fonts when present.");
    ui::card_end();
}

} // namespace panel
