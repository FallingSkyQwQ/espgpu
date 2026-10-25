/*
 * viewer.cpp — 帧查看器：把设备回传的帧上传成 D3D 纹理并画出来
 *
 * 这块 GPU 没有物理屏幕，Viewer 就是它的显示器。像素艺术式的 320x200 画面
 * 放大时必须用最近邻采样，否则会糊成一片 —— ImGui 的 DX11 后端默认是线性
 * 采样器，这里在 AddImage 前后插回调临时换成点采样器。
 */

#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "theme.h"
#include "widgets.h"

namespace panel {

namespace {

ID3D11ShaderResourceView *g_srv = nullptr;
ID3D11SamplerState *g_point = nullptr;
int g_w = 0;
int g_h = 0;
uint32_t g_frame_id = 0;
std::vector<uint8_t> g_rgba; /* 最近一帧，截图用 */
int g_rgba_w = 0, g_rgba_h = 0; /* g_rgba 的实际尺寸（纹理重建失败时可能与 g_w/g_h 不同） */

void set_point_sampler(const ImDrawList *, const ImDrawCmd *)
{
    if (g_point && g_app.ctx) {
        g_app.ctx->PSSetSamplers(0, 1, &g_point);
    }
}

} // namespace

bool viewer_init()
{
    if (g_app.d3d == nullptr) return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(g_app.d3d->CreateSamplerState(&sd, &g_point));
}

void viewer_shutdown()
{
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_point) { g_point->Release(); g_point = nullptr; }
    g_w = g_h = 0; /* 下次上传时按新设备重建纹理 */
}

bool viewer_has_frame() { return g_srv != nullptr; }
int viewer_width() { return g_w; }
int viewer_height() { return g_h; }

void viewer_upload()
{
    if (g_app.d3d == nullptr || g_app.ctx == nullptr) return;
    int w = 0, h = 0;
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lk(g_app.dev_mu);
        espgpu_device_t *dev = g_app.dev;
        if (!dev) return;
        const int w0 = espgpu_frame_width(dev);
        const int h0 = espgpu_frame_height(dev);
        if (w0 <= 0 || h0 <= 0) return;
        const size_t need = static_cast<size_t>(w0) * h0 * 4;
        /* 尺寸可能在两次调用之间变化，多留一点余量并靠返回值兜底 */
        if (g_rgba.size() < need) g_rgba.resize(need);
        if (espgpu_frame_snapshot(dev, g_rgba.data(), g_rgba.size(), &w, &h, &id) != 0) {
            return;
        }
    }
    if (w <= 0 || h <= 0) return;
    g_rgba_w = w;
    g_rgba_h = h;

    if (g_srv == nullptr || g_w != w || g_h != h) {
        if (g_srv) { g_srv->Release(); g_srv = nullptr; }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Texture2D *tex = nullptr;
        if (FAILED(g_app.d3d->CreateTexture2D(&desc, nullptr, &tex)) || !tex) return;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        const HRESULT hr = g_app.d3d->CreateShaderResourceView(tex, &srv, &g_srv);
        tex->Release();
        if (FAILED(hr)) { g_srv = nullptr; return; }
        g_w = w;
        g_h = h;
    }

    ID3D11Resource *res = nullptr;
    g_srv->GetResource(&res);
    if (!res) return;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(g_app.ctx->Map(res, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        const size_t row = static_cast<size_t>(w) * 4;
        for (int y = 0; y < h; y++) {
            memcpy(static_cast<uint8_t *>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                   g_rgba.data() + static_cast<size_t>(y) * row, row);
        }
        g_app.ctx->Unmap(res, 0);
    }
    res->Release();
    g_frame_id = id;
}

void viewer_draw(ImVec2 avail, bool hud)
{
    using theme::px;
    const theme::Scheme &s = theme::c();
    if (g_srv == nullptr || g_w <= 0 || g_h <= 0) {
        ui::empty_state(theme::icon::eye, "Waiting for the first frame",
                        "Frames appear here as soon as the device starts streaming - either the "
                        "built-in test pattern or a program drawing through espgpu.dll.");
        return;
    }

    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (avail.x < 16 || avail.y < 16) return;
    ImGui::InvisibleButton("##frame", avail);
    const bool hovered = ImGui::IsItemHovered();

    float scale = std::min(avail.x / g_w, avail.y / g_h);
    if (g_app.settings.viewer_integer && scale >= 1.0f) {
        scale = std::floor(scale);
    }
    const float dw = g_w * scale, dh = g_h * scale;
    const ImVec2 a(std::floor(p.x + (avail.x - dw) * 0.5f), std::floor(p.y + (avail.y - dh) * 0.5f));
    const ImVec2 b(a.x + dw, a.y + dh);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (!g_app.fullscreen) {
        dl->AddRectFilled(p, ImVec2(p.x + avail.x, p.y + avail.y),
                          theme::u32(s.surface_container_lowest), px(16));
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + avail.x, p.y + avail.y), IM_COL32(0, 0, 0, 255));
    }
    dl->AddCallback(set_point_sampler, nullptr);
    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_srv)), a, b);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    if (!g_app.fullscreen) {
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1),
                    theme::u32(s.outline_variant), 0.0f, 0, px(1));
    }

    /* 直播 / 停滞指示 */
    const double age = now_s() - g_app.last_frame_time.load();
    const bool live = age < 0.6;
    {
        ImFont *lf = theme::fonts().label;
        const char *txt = live ? "LIVE" : "STALLED";
        const ImVec2 ts = lf->CalcTextSizeA(lf->FontSize, FLT_MAX, 0, txt);
        const ImVec2 ca(b.x - ts.x - px(36), a.y + px(10));
        const ImVec2 cb(b.x - px(10), ca.y + px(24));
        dl->AddRectFilled(ca, cb, theme::u32(live ? s.error_container : s.surface_container_highest, 0.92f), px(12));
        dl->AddCircleFilled(ImVec2(ca.x + px(12), (ca.y + cb.y) * 0.5f), px(4),
                            theme::u32(live ? s.error : s.outline), 12);
        dl->AddText(lf, lf->FontSize, ImVec2(ca.x + px(20), ca.y + (px(24) - ts.y) * 0.5f),
                    theme::u32(live ? s.on_error_container : s.on_surface_variant), txt);
    }

    if (hud) {
        const Snapshot sn = snapshot();
        char lines[4][96];
        snprintf(lines[0], sizeof(lines[0]), "%d x %d   frame %u", g_w, g_h, g_frame_id);
        snprintf(lines[1], sizeof(lines[1]), "%.1f fps   %.0f KB/s", sn.link.fps, sn.link.in_kbps);
        const float ratio = sn.s.frame_enc_bytes ? static_cast<float>(sn.s.frame_raw_bytes) / sn.s.frame_enc_bytes : 0.0f;
        snprintf(lines[2], sizeof(lines[2]), "%u B/frame   %.1fx", sn.s.frame_enc_bytes, ratio);
        snprintf(lines[3], sizeof(lines[3]), "scale %.2fx %s", scale,
                 g_app.settings.viewer_integer ? "(integer)" : "(fit)");
        ImFont *lf = theme::fonts().label;
        float wmax = 0;
        for (auto &l : lines) wmax = std::max(wmax, lf->CalcTextSizeA(lf->FontSize, FLT_MAX, 0, l).x);
        const float lh = lf->FontSize + px(3);
        const ImVec2 ha(a.x + px(10), a.y + px(10));
        const ImVec2 hb(ha.x + wmax + px(20), ha.y + lh * 4 + px(14));
        dl->AddRectFilled(ha, hb, theme::u32(s.surface_container, 0.86f), px(12));
        for (int i = 0; i < 4; i++) {
            dl->AddText(lf, lf->FontSize, ImVec2(ha.x + px(10), ha.y + px(7) + lh * i),
                        theme::u32(i == 0 ? s.on_surface : s.on_surface_variant), lines[i]);
        }
    }

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        toggle_fullscreen();
    }
}

/** 截图目录：%USERPROFILE%\Pictures（存在时），否则 exe 所在目录。
 *  以前写进当前目录，从快捷方式启动时可能是 System32 之类不可写的位置。 */
static std::wstring shot_dir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
        const std::wstring d = std::wstring(buf) + L"\\Pictures";
        const DWORD a = GetFileAttributesW(d.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            return d + L"\\";
        }
    }
    n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
        std::wstring exe(buf, n);
        const size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) return exe.substr(0, slash + 1);
    }
    return {};
}

std::wstring viewer_screenshot()
{
    const int w = g_rgba_w, h = g_rgba_h;
    if (w <= 0 || h <= 0 || g_rgba.size() < static_cast<size_t>(w) * h * 4) {
        return {};
    }
    wchar_t name[96];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    wcsftime(name, 96, L"espgpu-shot-%Y%m%d-%H%M%S.bmp", &tmv);
    const std::wstring path = shot_dir() + name;

    const int row = (w * 3 + 3) & ~3;
    const uint32_t img = static_cast<uint32_t>(row) * h;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + img;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h; /* 正值 = 自底向上 */
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = img;

    FILE *f = _wfopen(path.c_str(), L"wb");
    if (!f) return {};
    bool ok = fwrite(&fh, sizeof(fh), 1, f) == 1 && fwrite(&ih, sizeof(ih), 1, f) == 1;
    std::vector<uint8_t> line(static_cast<size_t>(row), 0);
    for (int y = h - 1; y >= 0 && ok; y--) {
        const uint8_t *src = g_rgba.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; x++) {
            line[static_cast<size_t>(x) * 3 + 0] = src[x * 4 + 2];
            line[static_cast<size_t>(x) * 3 + 1] = src[x * 4 + 1];
            line[static_cast<size_t>(x) * 3 + 2] = src[x * 4 + 0];
        }
        ok = fwrite(line.data(), 1, line.size(), f) == line.size();
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        DeleteFileW(path.c_str()); /* 磁盘满等：不留半截文件 */
        return {};
    }

    wchar_t full[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH * 2, full, nullptr);
    if (n > 0 && n < MAX_PATH * 2) { /* n >= 容量时 full 未写入 */
        return full;
    }
    return path;
}

} // namespace panel
