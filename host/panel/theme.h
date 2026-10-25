/*
 * theme.h — Material 3 配色、字体、动画
 *
 * 配色按 M3 的「色调调色板」生成：从种子色取色相/色度，在 CIELAB 里按
 * tone（= L*）取样，再映射成各个色彩角色（primary / surfaceContainer… ）。
 * 这与 M3 官方的 HCT 思路一致（tone 就是 L*），只是色相/色度用 LCh 近似 CAM16。
 */

#pragma once

#include "imgui.h"

namespace theme {

/** M3 色彩角色。所有颜色都是线性混合前的 sRGB（0..1）。 */
struct Scheme {
    ImVec4 primary, on_primary, primary_container, on_primary_container;
    ImVec4 secondary, on_secondary, secondary_container, on_secondary_container;
    ImVec4 tertiary, on_tertiary, tertiary_container, on_tertiary_container;
    ImVec4 error, on_error, error_container, on_error_container;
    ImVec4 success, success_container, on_success_container; /* M3 没有，自定义扩展 */
    ImVec4 warning, warning_container, on_warning_container;
    ImVec4 surface, surface_dim, surface_bright;
    ImVec4 surface_container_lowest, surface_container_low, surface_container;
    ImVec4 surface_container_high, surface_container_highest;
    ImVec4 on_surface, on_surface_variant, outline, outline_variant;
    ImVec4 inverse_surface, inverse_on_surface, inverse_primary;
    ImVec4 scrim, shadow;
};

struct Seed {
    const char *name;
    float hue;    /* LCh 色相，度 */
    float chroma; /* LCh 色度 */
};

/** 内置种子色，外观页里可选 */
extern const Seed kSeeds[];
extern const int kSeedCount;

/** 当前（动画中）的配色。每帧由 update() 推进。 */
const Scheme &c();

/** 设置目标配色。切换时 250ms 渐变过去（reduce_motion 时立即生效）。 */
void set_target(bool dark, int seed_index);

/** 每帧调用：推进配色动画、把配色写进 ImGuiStyle。 */
void update(float dt);

/** 按 DPI 缩放重建 ImGuiStyle 的尺寸部分。 */
void apply_style_sizes(float scale);

/* ── 外观设置（由面板持久化） ─────────────────────────────────────── */

struct Appearance {
    bool dark = true;
    bool follow_system = false;
    int seed = 0;
    bool reduce_motion = false;
};

Appearance &appearance();

/** Windows 当前是否使用深色应用主题（读注册表）。 */
bool system_prefers_dark();

/* ── 字体 ─────────────────────────────────────────────────────────── */

struct Fonts {
    ImFont *body = nullptr;     /* 15 */
    ImFont *body_strong = nullptr;
    ImFont *label = nullptr;    /* 12.5 */
    ImFont *title = nullptr;    /* 20 */
    ImFont *headline = nullptr; /* 28 */
    ImFont *display = nullptr;  /* 40，大数字 */
    ImFont *icon = nullptr;     /* 18，Segoe Fluent Icons / MDL2 */
    ImFont *icon_large = nullptr;
    bool have_icons = false;
};

Fonts &fonts();

/** 重建字体图集（DPI 改变时调用；之后后端需要重建字体纹理）。 */
void load_fonts(float scale);

/* ── 图标（Segoe Fluent Icons / MDL2 码位）────────────────────────── */

namespace icon {
constexpr const char *home = "\xee\xa0\x8f"; /* E80F */
constexpr const char *info = "\xee\xa5\x86"; /* E946 */
constexpr const char *display = "\xee\x9f\xb4"; /* E7F4 */
constexpr const char *speed = "\xee\xb1\x8a"; /* EC4A */
constexpr const char *eye = "\xee\xa2\x90"; /* E890 */
constexpr const char *bug = "\xee\xaf\xa8"; /* EBE8 */
constexpr const char *memory = "\xee\x9e\xb8"; /* E7B8 */
constexpr const char *clock = "\xee\xa4\x97"; /* E917 */
constexpr const char *download = "\xee\xa2\x96"; /* E896 */
constexpr const char *log = "\xee\xa3\xbd"; /* E8FD */
constexpr const char *palette = "\xee\x9e\x90"; /* E790 */
constexpr const char *refresh = "\xee\x9c\xac"; /* E72C */
constexpr const char *save = "\xee\x9d\x8e"; /* E74E */
constexpr const char *fullscreen = "\xee\x9d\x80"; /* E740 */
constexpr const char *camera = "\xee\x9c\xa2"; /* E722 */
constexpr const char *warning = "\xee\x9e\xba"; /* E7BA */
constexpr const char *error = "\xee\x9e\x83"; /* E783 */
constexpr const char *check = "\xee\x9c\xbe"; /* E73E */
constexpr const char *cancel = "\xee\x9c\x91"; /* E711 */
constexpr const char *power = "\xee\x9f\xa8"; /* E7E8 */
constexpr const char *usb = "\xee\xa2\x8e"; /* E88E */
constexpr const char *chip = "\xee\xa5\x90"; /* E950 */
constexpr const char *brightness = "\xee\x9c\x86"; /* E706 */
constexpr const char *moon = "\xee\x9c\x88"; /* E708 */
constexpr const char *search = "\xee\x9c\xa1"; /* E721 */
constexpr const char *copy = "\xee\xa3\x88"; /* E8C8 */
constexpr const char *del = "\xee\x9d\x8d"; /* E74D */
constexpr const char *folder = "\xee\xa2\xb7"; /* E8B7 */
constexpr const char *heat = "\xee\xa7\x8a"; /* E9CA */
constexpr const char *link = "\xee\x9c\x9b"; /* E71B */
} // namespace icon

/* ── 颜色工具 ─────────────────────────────────────────────────────── */

ImU32 u32(const ImVec4 &v, float alpha_mul = 1.0f);
ImVec4 mix(const ImVec4 &a, const ImVec4 &b, float t);
ImVec4 with_alpha(const ImVec4 &v, float a);
/** M3 状态层：在 base 上叠加 on 色的 opacity（8% hover / 12% press）。 */
ImVec4 state_layer(const ImVec4 &base, const ImVec4 &on, float opacity);

/* ── 动画 ─────────────────────────────────────────────────────────── */

/** 帧间推进的标量动画，按 id 记忆。duration 秒；reduce_motion 时直接到目标。 */
float animate(ImGuiID id, float target, float duration = 0.18f);
/** M3 emphasized 缓动的近似（三次贝塞尔 0.2,0,0,1）。 */
float ease_emphasized(float t);
/** 全局时间（秒），用于呼吸/脉冲动画；reduce_motion 时冻结。 */
double time();

/** 当前 DPI 缩放 */
float dpi();
void set_dpi(float s);
inline float px(float v) { return v * dpi(); }

} // namespace theme
