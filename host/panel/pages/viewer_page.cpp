/*
 * viewer_page.cpp — 帧查看器页面：工具栏 + 画面
 */

#include "common.h"

namespace panel {

void page_viewer()
{
    if (!viewer_has_frame() && !require_device()) {
        return;
    }
    Settings &st = g_app.settings;

    static const char *const kScale[] = {"Fit", "Integer"};
    int sc = st.viewer_integer ? 1 : 0;
    if (ui::segmented("scale", kScale, 2, &sc, px(220))) {
        st.viewer_integer = sc == 1;
        save_settings();
    }
    ImGui::SameLine(0, px(16));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(4));
    if (ui::toggle("HUD", &st.viewer_hud)) {
        save_settings();
    }
    ImGui::SameLine(0, px(16));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(4));
    if (ui::button("Screenshot", ui::ButtonKind::Tonal, theme::icon::camera, 0,
                   viewer_has_frame())) {
        const std::wstring path = viewer_screenshot();
        if (path.empty()) {
            ui::snackbar(ui::SnackKind::Error, "Could not save screenshot");
        } else {
            char u8[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, u8, sizeof(u8), nullptr, nullptr);
            ui::snackbar(ui::SnackKind::Success, "Saved %s", u8);
        }
    }
    ImGui::SameLine(0, px(8));
    if (ui::button("Full screen", ui::ButtonKind::Outlined, theme::icon::fullscreen)) {
        toggle_fullscreen();
    }
    ImGui::SameLine(0, px(12));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(10));
    ui::text_label("F11 / double-click toggles full screen, Esc leaves it");

    ImGui::Dummy(ImVec2(0, px(4)));
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= px(8);
    if (avail.y < px(200)) avail.y = px(200);
    viewer_draw(avail, st.viewer_hud);
}

} // namespace panel
