/*
 * firmware.cpp — 固件信息与 USB OTA 升级
 *
 * espgpu_ota_flash 是阻塞调用（几十秒），放在设备线程上执行；
 * 进度通过回调写进 g_app.ota 的原子变量，UI 只读。
 */

#include "common.h"

#include <commdlg.h>

namespace panel {

namespace {

bool ota_progress(uint32_t done, uint32_t total, void *user)
{
    auto *o = static_cast<OtaState *>(user);
    o->done = done;
    o->total = total;
    return !o->cancel.load();
}

bool pick_file(std::wstring &out)
{
    /* MAX_PATH 放不下深目录里的路径（对话框会直接以 FNERR_BUFFERTOOSMALL 失败） */
    static wchar_t path[4096];
    path[0] = L'\0';
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app.hwnd;
    ofn.lpstrFilter = L"Firmware image (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    ofn.lpstrTitle = L"Select espgpu firmware image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }
    out = path;
    return true;
}

bool load_file(const std::wstring &path, std::vector<uint8_t> &data)
{
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    data.resize(static_cast<size_t>(n));
    const size_t got = fread(data.data(), 1, data.size(), f);
    fclose(f);
    return got == data.size();
}

char g_version[64] = "";

} // namespace

void page_firmware()
{
    if (!require_device()) {
        return;
    }
    const theme::Scheme &s = theme::c();
    const Snapshot sn = snapshot();
    const espgpu_device_info_t &i = sn.info;
    OtaState &o = g_app.ota;

    ui::card_begin("cur", 0, ui::CardKind::Filled);
    card_title("Installed firmware");
    if (ui::kv_begin("fwkv")) {
        ui::kv_row("Version", "%u.%u.%u", i.fw_major, i.fw_minor, i.fw_patch);
        ui::kv_row("Build", "%08X", i.fw_git);
        ui::kv_row("Protocol", "%u", i.proto_version);
        ui::kv_row("Chip", "%s", i.chip_model);
        ui::kv_end();
    }
    ui::card_end();

    if (!require_cap(i, ESPGPU_CAP_OTA, "USB firmware update")) {
        return;
    }

    ui::card_begin("ota", 0);
    card_title("Update over USB",
               "Writes the image to the inactive OTA partition and switches the boot partition. "
               "If the new firmware fails to boot, the bootloader rolls back.");

    const bool running = o.running.load();
    if (ui::button("Choose image…", ui::ButtonKind::Outlined, theme::icon::folder, 0, !running)) {
        std::wstring p;
        if (pick_file(p)) {
            std::vector<uint8_t> data;
            if (load_file(p, data)) {
                o.file_path = p;
                o.image = std::move(data);
                o.result = 0;
                o.needs_reboot = false;
                o.done = 0;
                o.total = static_cast<uint32_t>(o.image.size());
                /* 默认版本串取文件名 */
                const size_t slash = p.find_last_of(L"\\/");
                const std::string nm = narrow(slash == std::wstring::npos ? p : p.substr(slash + 1));
                snprintf(g_version, sizeof(g_version), "%s", nm.c_str());
            } else {
                ui::snackbar(ui::SnackKind::Error, "Could not read the image file");
            }
        }
    }

    if (!o.image.empty()) {
        ImGui::SameLine(0, px(12));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(10));
        char sz[32];
        fmt_bytes(sz, sizeof(sz), static_cast<double>(o.image.size()));
        ImGui::Text("%s  (%s)", narrow(o.file_path).c_str(), sz);

        /* ESP 应用镜像的第一个字节是 0xE9 */
        if (o.image[0] != 0xE9) {
            ui::text_colored(s.warning, "%s This does not look like an ESP-IDF app image "
                                        "(missing 0xE9 magic).",
                             theme::fonts().have_icons ? theme::icon::warning : "!");
        }
        ImGui::Dummy(ImVec2(0, px(2)));
        ui::text_label("Version label (sent to the device)");
        ImGui::BeginDisabled(running);
        ui::text_field("ver", "e.g. 0.2.0", g_version, sizeof(g_version), nullptr, px(320));
        ImGui::EndDisabled();
    }

    if (running || o.result.load() != 0 || o.done.load() > 0) {
        ImGui::Dummy(ImVec2(0, px(4)));
        const uint32_t done = o.done.load(), total = o.total.load();
        const float frac = total ? static_cast<float>(done) / total : 0.0f;
        const int res = o.result.load();
        const ImVec4 col = res < 0 ? s.error : (res > 0 ? s.success : s.primary);
        ui::progress(frac, 0, &col);
        const char *state = running ? "Writing…" : res > 0 ? "Done" : res == -2 ? "Cancelled" : res < 0 ? "Failed" : "";
        ImGui::Text("%s  %u / %u KB  (%.0f%%)", state, done / 1024, total / 1024, frac * 100.0f);
    }

    ImGui::Dummy(ImVec2(0, px(4)));
    if (running) {
        if (ui::button("Cancel", ui::ButtonKind::Text, theme::icon::cancel)) {
            o.cancel = true;
        }
    } else {
        if (ui::button("Flash firmware", ui::ButtonKind::Filled, theme::icon::download, 0,
                       !o.image.empty())) {
            ui::open_dialog("Flash firmware?");
        }
        if (o.needs_reboot.load()) {
            ImGui::SameLine(0, px(8));
            if (ui::button("Reboot now", ui::ButtonKind::Tonal, theme::icon::power)) {
                enqueue("Reboot", [](espgpu_device_t *dev) { return espgpu_soft_reset(dev, 800); },
                        "Device rebooting into the new firmware…");
                o.needs_reboot = false;
            }
        }
    }
    if (sn.valid && sn.s.ota_state) {
        static const char *const kOta[] = {"idle", "writing", "pending reboot", "failed"};
        ui::text_label(sn.s.ota_state < 4 ? kOta[sn.s.ota_state] : "unknown");
    }
    ui::card_end();

    if (ui::dialog("Flash firmware?", theme::icon::download, "Flash firmware?",
                   "Do not unplug the board while writing. Rendering stops during the update.",
                   "Flash", true) == 1 &&
        !o.image.empty() && !o.running.load()) {
        o.running = true;
        o.cancel = false;
        o.done = 0;
        o.total = static_cast<uint32_t>(o.image.size());
        o.result = 0;
        o.version = g_version;
        /* 直接投递：未连接时也必须清掉 running，否则界面永远停在 "Writing…"，
         * 取消也要和失败区分开（原来取消会弹 "device error" 的报错） */
        enqueue_raw([](espgpu_device_t *dev) {
            OtaState &os = g_app.ota;
            if (dev == nullptr) {
                os.result = -1;
                os.running = false;
                ui::snackbar(ui::SnackKind::Error, "Firmware update: device not connected");
                return;
            }
            log_line(3, false, "firmware update: %zu bytes", os.image.size());
            const int rc = espgpu_ota_flash(dev, os.image.data(), os.image.size(),
                                            os.version.c_str(), ota_progress, &os);
            if (rc == 0) {
                os.needs_reboot = true;
                os.done = os.total.load();
                os.result = 1;
                ui::snackbar(ui::SnackKind::Success, "Firmware written - reboot to apply");
                log_line(3, false, "firmware update: ok");
            } else if (os.cancel.load()) {
                os.result = -2;
                ui::snackbar(ui::SnackKind::Info, "Firmware update cancelled");
                log_line(2, false, "firmware update cancelled");
            } else {
                os.result = -1;
                ui::snackbar(ui::SnackKind::Error, "Firmware update failed (status %d)", rc);
                log_line(1, false, "firmware update failed: status %d", rc);
            }
            os.running = false;
        });
    }
}

} // namespace panel
