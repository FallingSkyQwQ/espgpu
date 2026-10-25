/*
 * theme.cpp — Material 3 配色生成、ImGuiStyle 映射、字体、动画
 */

#include "theme.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace theme {

/* ════════════════════════════════════════════════════════════════════
 * 色调调色板（CIELAB LCh，tone = L*）
 * ════════════════════════════════════════════════════════════════════ */

namespace {

constexpr float kPi = 3.14159265358979f;

float srgb_gamma(float v)
{
    if (v <= 0.0031308f) {
        return 12.92f * v;
    }
    return 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

/** Lab → sRGB，返回是否在色域内。 */
bool lab_to_srgb(float L, float a, float b, float out[3])
{
    /* Lab → XYZ（D65） */
    const float fy = (L + 16.0f) / 116.0f;
    const float fx = fy + a / 500.0f;
    const float fz = fy - b / 200.0f;
    auto finv = [](float t) {
        const float d = 6.0f / 29.0f;
        return t > d ? t * t * t : 3.0f * d * d * (t - 4.0f / 29.0f);
    };
    const float X = 0.95047f * finv(fx);
    const float Y = 1.00000f * finv(fy);
    const float Z = 1.08883f * finv(fz);

    /* XYZ → 线性 sRGB */
    const float r = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
    const float g = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
    const float bl = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;

    const float eps = 0.0005f;
    const bool in = r >= -eps && r <= 1 + eps && g >= -eps && g <= 1 + eps && bl >= -eps &&
                    bl <= 1 + eps;
    auto cl = [](float v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); };
    out[0] = srgb_gamma(cl(r));
    out[1] = srgb_gamma(cl(g));
    out[2] = srgb_gamma(cl(bl));
    return in;
}

/** 在给定色相 / 色度 / tone 取一个颜色；超出色域时降色度（保持 tone 与色相）。 */
ImVec4 tone(float hue_deg, float chroma, float t)
{
    if (t >= 100.0f) {
        return ImVec4(1, 1, 1, 1);
    }
    if (t <= 0.0f) {
        return ImVec4(0, 0, 0, 1);
    }
    const float h = hue_deg * kPi / 180.0f;
    float rgb[3];
    if (lab_to_srgb(t, chroma * std::cos(h), chroma * std::sin(h), rgb)) {
        return ImVec4(rgb[0], rgb[1], rgb[2], 1);
    }
    float lo = 0.0f, hi = chroma;
    for (int i = 0; i < 18; i++) {
        const float mid = (lo + hi) * 0.5f;
        if (lab_to_srgb(t, mid * std::cos(h), mid * std::sin(h), rgb)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    lab_to_srgb(t, lo * std::cos(h), lo * std::sin(h), rgb);
    return ImVec4(rgb[0], rgb[1], rgb[2], 1);
}

struct Palette {
    float hue, chroma;
    ImVec4 operator()(float t) const { return tone(hue, chroma, t); }
};

Scheme make_scheme(const Seed &seed, bool dark)
{
    const Palette P{seed.hue, seed.chroma < 48.0f ? 48.0f : seed.chroma};
    const Palette S{seed.hue, 16.0f};
    const Palette T{seed.hue + 60.0f, 24.0f};
    const Palette E{25.0f, 84.0f};
    const Palette G{140.0f, 48.0f}; /* success */
    const Palette W{75.0f, 60.0f};  /* warning */
    const Palette N{seed.hue, 4.0f};
    const Palette NV{seed.hue, 8.0f};

    Scheme s{};
    if (dark) {
        s.primary = P(80); s.on_primary = P(20); s.primary_container = P(30); s.on_primary_container = P(90);
        s.secondary = S(80); s.on_secondary = S(20); s.secondary_container = S(30); s.on_secondary_container = S(90);
        s.tertiary = T(80); s.on_tertiary = T(20); s.tertiary_container = T(30); s.on_tertiary_container = T(90);
        s.error = E(80); s.on_error = E(20); s.error_container = E(30); s.on_error_container = E(90);
        s.success = G(80); s.success_container = G(30); s.on_success_container = G(90);
        s.warning = W(80); s.warning_container = W(30); s.on_warning_container = W(90);
        s.surface = N(6); s.surface_dim = N(6); s.surface_bright = N(24);
        s.surface_container_lowest = N(4); s.surface_container_low = N(10);
        s.surface_container = N(12); s.surface_container_high = N(17);
        s.surface_container_highest = N(22);
        s.on_surface = N(90); s.on_surface_variant = NV(80);
        s.outline = NV(60); s.outline_variant = NV(30);
        s.inverse_surface = N(90); s.inverse_on_surface = N(20); s.inverse_primary = P(40);
    } else {
        s.primary = P(40); s.on_primary = P(100); s.primary_container = P(90); s.on_primary_container = P(10);
        s.secondary = S(40); s.on_secondary = S(100); s.secondary_container = S(90); s.on_secondary_container = S(10);
        s.tertiary = T(40); s.on_tertiary = T(100); s.tertiary_container = T(90); s.on_tertiary_container = T(10);
        s.error = E(40); s.on_error = E(100); s.error_container = E(90); s.on_error_container = E(10);
        s.success = G(40); s.success_container = G(90); s.on_success_container = G(10);
        s.warning = W(45); s.warning_container = W(90); s.on_warning_container = W(10);
        s.surface = N(98); s.surface_dim = N(87); s.surface_bright = N(98);
        s.surface_container_lowest = N(100); s.surface_container_low = N(96);
        s.surface_container = N(94); s.surface_container_high = N(92);
        s.surface_container_highest = N(90);
        s.on_surface = N(10); s.on_surface_variant = NV(30);
        s.outline = NV(50); s.outline_variant = NV(80);
        s.inverse_surface = N(20); s.inverse_on_surface = N(95); s.inverse_primary = P(80);
    }
    s.scrim = ImVec4(0, 0, 0, 1);
    s.shadow = ImVec4(0, 0, 0, 1);
    return s;
}

Scheme lerp_scheme(const Scheme &a, const Scheme &b, float t)
{
    Scheme r;
    const ImVec4 *pa = reinterpret_cast<const ImVec4 *>(&a);
    const ImVec4 *pb = reinterpret_cast<const ImVec4 *>(&b);
    ImVec4 *pr = reinterpret_cast<ImVec4 *>(&r);
    const int n = static_cast<int>(sizeof(Scheme) / sizeof(ImVec4));
    for (int i = 0; i < n; i++) {
        pr[i] = mix(pa[i], pb[i], t);
    }
    return r;
}

Scheme g_from{}, g_to{}, g_cur{};
float g_anim = 1.0f;
bool g_inited = false;

Appearance g_appearance;
Fonts g_fonts;
float g_dpi = 1.0f;
double g_time = 0.0;

struct AnimState {
    float value;
    float from;
    float target;
    float t;
    int last_frame;
};
std::unordered_map<ImGuiID, AnimState> g_anims;

} // namespace

const Seed kSeeds[] = {
    {"Teal", 200.0f, 42.0f},
    {"Violet", 300.0f, 52.0f},
    {"Azure", 255.0f, 50.0f},
    {"Jade", 155.0f, 44.0f},
    {"Amber", 75.0f, 56.0f},
    {"Rose", 0.0f, 54.0f},
};
const int kSeedCount = static_cast<int>(sizeof(kSeeds) / sizeof(kSeeds[0]));

const Scheme &c()
{
    if (!g_inited) {
        g_cur = g_to = g_from = make_scheme(kSeeds[0], true);
        g_inited = true;
    }
    return g_cur;
}

void set_target(bool dark, int seed_index)
{
    if (seed_index < 0 || seed_index >= kSeedCount) {
        seed_index = 0;
    }
    c();
    g_from = g_cur;
    g_to = make_scheme(kSeeds[seed_index], dark);
    g_anim = g_appearance.reduce_motion ? 1.0f : 0.0f;
    if (g_anim >= 1.0f) {
        g_cur = g_to;
    }
}

Appearance &appearance()
{
    return g_appearance;
}

bool system_prefers_dark()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) ==
        ERROR_SUCCESS) {
        return value == 0;
    }
    return true;
}

/* ── 颜色工具 ─────────────────────────────────────────────────────── */

ImU32 u32(const ImVec4 &v, float alpha_mul)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, v.w * alpha_mul));
}

ImVec4 mix(const ImVec4 &a, const ImVec4 &b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

ImVec4 with_alpha(const ImVec4 &v, float a)
{
    return ImVec4(v.x, v.y, v.z, a);
}

ImVec4 state_layer(const ImVec4 &base, const ImVec4 &on, float opacity)
{
    ImVec4 r = mix(base, on, opacity);
    r.w = base.w;
    return r;
}

/* ── 动画 ─────────────────────────────────────────────────────────── */

float ease_emphasized(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    /* 三次贝塞尔 (0.2, 0, 0, 1)：用牛顿法从 x 解 t 再求 y */
    const float x1 = 0.2f, x2 = 0.0f, y1 = 0.0f, y2 = 1.0f;
    float u = t;
    for (int i = 0; i < 6; i++) {
        const float iu = 1.0f - u;
        const float x = 3 * iu * iu * u * x1 + 3 * iu * u * u * x2 + u * u * u;
        const float dx = 3 * iu * iu * x1 + 6 * iu * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (std::fabs(dx) < 1e-5f) break;
        u -= (x - t) / dx;
        if (u < 0) u = 0;
        if (u > 1) u = 1;
    }
    const float iu = 1.0f - u;
    return 3 * iu * iu * u * y1 + 3 * iu * u * u * y2 + u * u * u;
}

float animate(ImGuiID id, float target, float duration)
{
    const int frame = ImGui::GetFrameCount();
    auto it = g_anims.find(id);
    if (it == g_anims.end()) {
        g_anims[id] = AnimState{target, target, target, 1.0f, frame};
        return target;
    }
    AnimState &a = it->second;
    if (a.target != target) {
        a.from = a.value;
        a.target = target;
        a.t = 0.0f;
    }
    if (a.last_frame != frame) {
        a.last_frame = frame;
        if (g_appearance.reduce_motion || duration <= 0.0f) {
            a.t = 1.0f;
        } else {
            a.t += ImGui::GetIO().DeltaTime / duration;
        }
        if (a.t > 1.0f) a.t = 1.0f;
        a.value = a.from + (a.target - a.from) * ease_emphasized(a.t);
    }
    return a.value;
}

double time()
{
    return g_time;
}

/* ── 每帧推进 ─────────────────────────────────────────────────────── */

static void write_style_colors()
{
    const Scheme &s = g_cur;
    ImVec4 *col = ImGui::GetStyle().Colors;

    col[ImGuiCol_Text] = s.on_surface;
    col[ImGuiCol_TextDisabled] = with_alpha(s.on_surface_variant, 0.75f);
    col[ImGuiCol_WindowBg] = s.surface;
    col[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_PopupBg] = s.surface_container_high;
    col[ImGuiCol_Border] = s.outline_variant;
    col[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_FrameBg] = s.surface_container_highest;
    col[ImGuiCol_FrameBgHovered] = state_layer(s.surface_container_highest, s.on_surface, 0.08f);
    col[ImGuiCol_FrameBgActive] = state_layer(s.surface_container_highest, s.on_surface, 0.12f);
    col[ImGuiCol_TitleBg] = s.surface_container;
    col[ImGuiCol_TitleBgActive] = s.surface_container_high;
    col[ImGuiCol_TitleBgCollapsed] = s.surface_container;
    col[ImGuiCol_MenuBarBg] = s.surface_container;
    col[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ScrollbarGrab] = with_alpha(s.on_surface_variant, 0.28f);
    col[ImGuiCol_ScrollbarGrabHovered] = with_alpha(s.on_surface_variant, 0.45f);
    col[ImGuiCol_ScrollbarGrabActive] = with_alpha(s.primary, 0.7f);
    col[ImGuiCol_CheckMark] = s.primary;
    col[ImGuiCol_SliderGrab] = s.primary;
    col[ImGuiCol_SliderGrabActive] = s.primary;
    col[ImGuiCol_Button] = s.secondary_container;
    col[ImGuiCol_ButtonHovered] = state_layer(s.secondary_container, s.on_secondary_container, 0.08f);
    col[ImGuiCol_ButtonActive] = state_layer(s.secondary_container, s.on_secondary_container, 0.12f);
    col[ImGuiCol_Header] = s.secondary_container;
    col[ImGuiCol_HeaderHovered] = state_layer(s.surface_container_high, s.on_surface, 0.08f);
    col[ImGuiCol_HeaderActive] = state_layer(s.secondary_container, s.on_surface, 0.12f);
    col[ImGuiCol_Separator] = s.outline_variant;
    col[ImGuiCol_SeparatorHovered] = s.outline;
    col[ImGuiCol_SeparatorActive] = s.primary;
    col[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ResizeGripHovered] = with_alpha(s.primary, 0.5f);
    col[ImGuiCol_ResizeGripActive] = s.primary;
    col[ImGuiCol_Tab] = s.surface_container;
    col[ImGuiCol_TabHovered] = state_layer(s.surface_container, s.on_surface, 0.08f);
    col[ImGuiCol_TabSelected] = s.secondary_container;
    col[ImGuiCol_TabSelectedOverline] = s.primary;
    col[ImGuiCol_TabDimmed] = s.surface_container;
    col[ImGuiCol_TabDimmedSelected] = s.surface_container_high;
    col[ImGuiCol_TabDimmedSelectedOverline] = s.outline;
    col[ImGuiCol_PlotLines] = s.primary;
    col[ImGuiCol_PlotLinesHovered] = s.tertiary;
    col[ImGuiCol_PlotHistogram] = s.primary;
    col[ImGuiCol_PlotHistogramHovered] = s.tertiary;
    col[ImGuiCol_TableHeaderBg] = s.surface_container_high;
    col[ImGuiCol_TableBorderStrong] = s.outline_variant;
    col[ImGuiCol_TableBorderLight] = with_alpha(s.outline_variant, 0.6f);
    col[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_TableRowBgAlt] = with_alpha(s.on_surface, 0.035f);
    col[ImGuiCol_TextLink] = s.primary;
    col[ImGuiCol_TextSelectedBg] = with_alpha(s.primary, 0.35f);
    col[ImGuiCol_DragDropTarget] = s.tertiary;
    col[ImGuiCol_NavCursor] = s.primary;
    col[ImGuiCol_NavWindowingHighlight] = with_alpha(s.primary, 0.7f);
    col[ImGuiCol_NavWindowingDimBg] = with_alpha(s.scrim, 0.32f);
    col[ImGuiCol_ModalWindowDimBg] = with_alpha(s.scrim, 0.42f);
}

void update(float dt)
{
    c();
    if (!g_appearance.reduce_motion) {
        g_time += dt;
    }
    if (g_anim < 1.0f) {
        g_anim += dt / 0.25f;
        if (g_anim >= 1.0f || g_appearance.reduce_motion) {
            g_anim = 1.0f;
        }
        g_cur = lerp_scheme(g_from, g_to, ease_emphasized(g_anim));
    }
    write_style_colors();
}

void apply_style_sizes(float scale)
{
    ImGuiStyle &st = ImGui::GetStyle();
    ImGuiStyle fresh;
    /* 只重置尺寸字段，颜色每帧由 write_style_colors 写 */
    ImVec4 colors[ImGuiCol_COUNT];
    memcpy(colors, st.Colors, sizeof(colors));
    st = fresh;
    memcpy(st.Colors, colors, sizeof(colors));

    st.WindowPadding = ImVec2(0, 0);
    st.WindowRounding = 0.0f;
    st.WindowBorderSize = 0.0f;
    st.ChildRounding = 12.0f;
    st.ChildBorderSize = 0.0f;
    st.PopupRounding = 12.0f;
    st.PopupBorderSize = 0.0f;
    st.FramePadding = ImVec2(12, 8);
    st.FrameRounding = 8.0f;
    st.FrameBorderSize = 0.0f;
    st.ItemSpacing = ImVec2(10, 10);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.CellPadding = ImVec2(10, 6);
    st.IndentSpacing = 20.0f;
    st.ScrollbarSize = 10.0f;
    st.ScrollbarRounding = 8.0f;
    st.GrabMinSize = 12.0f;
    st.GrabRounding = 8.0f;
    st.TabRounding = 8.0f;
    st.SeparatorTextBorderSize = 1.0f;
    st.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    st.AntiAliasedFill = true;
    st.AntiAliasedLines = true;
    st.ScaleAllSizes(scale);
}

/* ── 字体 ─────────────────────────────────────────────────────────── */

Fonts &fonts()
{
    return g_fonts;
}

static bool file_exists(const char *path)
{
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string font_path(const char *name)
{
    char windir[MAX_PATH] = "C:\\Windows";
    GetWindowsDirectoryA(windir, MAX_PATH);
    std::string p = std::string(windir) + "\\Fonts\\" + name;
    if (file_exists(p.c_str())) {
        return p;
    }
    /* 用户级安装的字体 */
    char local[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH)) {
        p = std::string(local) + "\\Microsoft\\Windows\\Fonts\\" + name;
        if (file_exists(p.c_str())) {
            return p;
        }
    }
    return {};
}

void load_fonts(float scale)
{
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    g_fonts = Fonts{};

    const std::string text_font = [] {
        for (const char *n : {"SegUIVar.ttf", "segoeui.ttf"}) {
            std::string p = font_path(n);
            if (!p.empty()) return p;
        }
        return std::string();
    }();
    const std::string strong_font = [] {
        for (const char *n : {"seguisb.ttf", "SegUIVar.ttf", "segoeui.ttf"}) {
            std::string p = font_path(n);
            if (!p.empty()) return p;
        }
        return std::string();
    }();
    const std::string cjk_font = [] {
        for (const char *n : {"msyh.ttc", "msyhl.ttc", "simsun.ttc"}) {
            std::string p = font_path(n);
            if (!p.empty()) return p;
        }
        return std::string();
    }();
    const std::string icon_font = [] {
        for (const char *n : {"SegoeIcons.ttf", "segmdl2.ttf"}) {
            std::string p = font_path(n);
            if (!p.empty()) return p;
        }
        return std::string();
    }();

    static const ImWchar kIconRanges[] = {0xE700, 0xF8FF, 0};
    static const ImWchar kExtraRanges[] = {0x0020, 0x00FF, 0x2010, 0x205E, 0x2190, 0x21FF,
                                           0x2200, 0x22FF, 0x25A0, 0x25FF, 0x00B0, 0x00B0, 0};

    auto add = [&](const std::string &path, float size, bool merge_cjk) -> ImFont * {
        ImFont *f = nullptr;
        const float px_size = std::floor(size * scale + 0.5f);
        if (!path.empty()) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 2;
            cfg.PixelSnapH = false;
            f = io.Fonts->AddFontFromFileTTF(path.c_str(), px_size, &cfg, kExtraRanges);
        }
        if (f == nullptr) {
            ImFontConfig cfg;
            cfg.SizePixels = px_size;
            f = io.Fonts->AddFontDefault(&cfg);
        }
        if (merge_cjk && !cjk_font.empty()) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.OversampleH = 1;
            cfg.OversampleV = 1;
            io.Fonts->AddFontFromFileTTF(cjk_font.c_str(), px_size, &cfg,
                                         io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }
        /* 正文字体里也合并图标，按钮里可以图标 + 文字混排 */
        if (!icon_font.empty()) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.GlyphMinAdvanceX = px_size;
            cfg.GlyphOffset = ImVec2(0, std::floor(px_size * 0.12f));
            io.Fonts->AddFontFromFileTTF(icon_font.c_str(), px_size * 0.95f, &cfg, kIconRanges);
        }
        return f;
    };

    g_fonts.body = add(text_font, 15.0f, true); /* 第一个即默认字体 */
    g_fonts.body_strong = add(strong_font, 15.0f, true);
    g_fonts.label = add(strong_font, 12.5f, true);
    g_fonts.title = add(strong_font, 20.0f, true);
    g_fonts.headline = add(text_font, 28.0f, false);
    g_fonts.display = add(text_font, 40.0f, false);

    if (!icon_font.empty()) {
        ImFontConfig cfg;
        g_fonts.icon = io.Fonts->AddFontFromFileTTF(icon_font.c_str(),
                                                    std::floor(18.0f * scale + 0.5f), &cfg,
                                                    kIconRanges);
        ImFontConfig cfg2;
        g_fonts.icon_large = io.Fonts->AddFontFromFileTTF(
            icon_font.c_str(), std::floor(40.0f * scale + 0.5f), &cfg2, kIconRanges);
        g_fonts.have_icons = g_fonts.icon != nullptr && g_fonts.icon_large != nullptr;
    }
    if (!g_fonts.icon) g_fonts.icon = g_fonts.body;
    if (!g_fonts.icon_large) g_fonts.icon_large = g_fonts.headline;

    io.FontDefault = g_fonts.body;
    io.Fonts->Build();
}

/* ── DPI ──────────────────────────────────────────────────────────── */

float dpi()
{
    return g_dpi;
}

void set_dpi(float s)
{
    g_dpi = s > 0.5f ? s : 1.0f;
}

} // namespace theme
