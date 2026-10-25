/*
 * widgets.cpp — Material 3 组件
 *
 * 全部自绘：InvisibleButton 负责交互，绘制走当前窗口的 draw list。
 * 颜色一律取 theme::c() 的角色色，动画走 theme::animate()。
 */

#include "widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using theme::c;
using theme::px;
using theme::u32;

namespace ui {

namespace {

/** "label##id" → 可见部分的结尾 */
const char *visible_end(const char *s)
{
    const char *p = strstr(s, "##");
    return p ? p : s + strlen(s);
}

ImVec2 text_size(ImFont *f, const char *b, const char *e = nullptr)
{
    if (e == nullptr) e = visible_end(b);
    return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, b, e);
}

void draw_text(ImDrawList *dl, ImFont *f, ImVec2 pos, const ImVec4 &col, const char *b,
               const char *e = nullptr)
{
    if (e == nullptr) e = visible_end(b);
    dl->AddText(f, f->FontSize, ImVec2(std::floor(pos.x), std::floor(pos.y)), u32(col), b, e);
}

void draw_icon(ImDrawList *dl, ImVec2 center, const char *icon, const ImVec4 &col,
               ImFont *font = nullptr)
{
    if (icon == nullptr) return;
    ImFont *f = font ? font : theme::fonts().icon;
    if (!theme::fonts().have_icons) {
        /* 没有图标字体：画一个小圆点占位 */
        dl->AddCircleFilled(center, px(3.0f), u32(col));
        return;
    }
    const ImVec2 sz = text_size(f, icon);
    draw_text(dl, f, ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f), col, icon);
}

float frame_dt()
{
    return ImGui::GetIO().DeltaTime;
}

} // namespace

/* ── 文本 ─────────────────────────────────────────────────────────── */

void text_headline(const char *s)
{
    ImGui::PushFont(theme::fonts().headline);
    ImGui::TextUnformatted(s);
    ImGui::PopFont();
}

void text_title(const char *s)
{
    ImGui::PushFont(theme::fonts().title);
    ImGui::TextUnformatted(s);
    ImGui::PopFont();
}

void text_label(const char *s)
{
    ImGui::PushFont(theme::fonts().label);
    ImGui::PushStyleColor(ImGuiCol_Text, c().on_surface_variant);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void text_body_variant(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, c().on_surface_variant);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void text_colored(const ImVec4 &col, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
}

void section(const char *s)
{
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::PushFont(theme::fonts().body_strong);
    ImGui::PushStyleColor(ImGuiCol_Text, c().primary);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/* ── 按钮 ─────────────────────────────────────────────────────────── */

bool button(const char *label, ButtonKind kind, const char *icon, float width, bool enabled)
{
    const theme::Scheme &s = c();
    ImFont *f = theme::fonts().body_strong;
    const float h = px(40);
    const ImVec2 ts = text_size(f, label);
    const bool has_text = ts.x > 0.0f;
    const float icon_w = icon ? px(18) : 0.0f;
    float w = width > 0 ? width
                        : (icon ? px(16) + icon_w + (has_text ? px(8) : 0) : px(24)) + ts.x + px(24);
    if (!has_text && icon) w = width > 0 ? width : h;

    ImGui::PushID(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##btn", ImVec2(w, h)) && enabled;
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool active = enabled && ImGui::IsItemActive();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float hv = theme::animate(ImGui::GetID("##h"), hovered ? 1.0f : 0.0f, 0.15f);
    const float pr = theme::animate(ImGui::GetID("##p"), active ? 1.0f : 0.0f, 0.10f);

    ImVec4 bg, fg;
    bool outline = false;
    switch (kind) {
    case ButtonKind::Filled:
        bg = s.primary; fg = s.on_primary;
        break;
    case ButtonKind::Danger:
        bg = s.error; fg = s.on_error;
        break;
    case ButtonKind::Tonal:
        bg = s.secondary_container; fg = s.on_secondary_container;
        break;
    case ButtonKind::Outlined:
        bg = theme::with_alpha(s.primary, 0.0f); fg = s.primary; outline = true;
        break;
    case ButtonKind::Text:
    default:
        bg = theme::with_alpha(s.primary, 0.0f); fg = s.primary;
        break;
    }
    if (!enabled) {
        bg = (kind == ButtonKind::Text || kind == ButtonKind::Outlined)
                 ? theme::with_alpha(s.on_surface, 0.0f)
                 : theme::with_alpha(s.on_surface, 0.12f);
        fg = theme::with_alpha(s.on_surface, 0.38f);
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    const float r = h * 0.5f;
    if (bg.w > 0.0f) {
        dl->AddRectFilled(p, q, u32(bg), r);
    }
    /* 状态层：hover 8%、press 12% */
    const float layer = 0.08f * hv + 0.04f * pr;
    if (layer > 0.001f && enabled) {
        dl->AddRectFilled(p, q, u32(fg, layer), r);
    }
    if (outline) {
        dl->AddRect(p, q, u32(enabled ? s.outline : theme::with_alpha(s.on_surface, 0.12f)), r,
                    0, px(1.0f));
    }

    float x = p.x + (w - ((icon ? icon_w + (has_text ? px(8) : 0) : 0) + ts.x)) * 0.5f;
    if (icon) {
        draw_icon(dl, ImVec2(x + icon_w * 0.5f, p.y + h * 0.5f), icon, fg);
        x += icon_w + (has_text ? px(8) : 0);
    }
    if (has_text) {
        draw_text(dl, f, ImVec2(x, p.y + (h - ts.y) * 0.5f), fg, label);
    }

    ImGui::PopID();
    return clicked;
}

bool icon_button(const char *id, const char *icon, const char *tip, bool selected)
{
    const theme::Scheme &s = c();
    const float d = px(40);
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##ib", ImVec2(d, d));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float hv = theme::animate(ImGui::GetID("##h"), hovered ? 1.0f : 0.0f, 0.15f);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 ctr(p.x + d * 0.5f, p.y + d * 0.5f);
    const ImVec4 fg = selected ? s.on_secondary_container : s.on_surface_variant;
    if (selected) {
        dl->AddCircleFilled(ctr, d * 0.5f, u32(s.secondary_container), 32);
    }
    const float layer = 0.08f * hv + (active ? 0.04f : 0.0f);
    if (layer > 0.001f) {
        dl->AddCircleFilled(ctr, d * 0.5f, u32(fg, layer), 32);
    }
    if (theme::fonts().have_icons) {
        draw_icon(dl, ctr, icon, fg);
    } else if (tip) {
        char ch[2] = {tip[0], 0};
        const ImVec2 ts = text_size(theme::fonts().body_strong, ch);
        draw_text(dl, theme::fonts().body_strong, ImVec2(ctr.x - ts.x * 0.5f, ctr.y - ts.y * 0.5f),
                  fg, ch);
    }
    if (tip) tooltip(tip);
    ImGui::PopID();
    return clicked;
}

/* ── 分段按钮 ─────────────────────────────────────────────────────── */

bool segmented(const char *id, const char *const *items, int count, int *selected, float width)
{
    if (count <= 0) return false;
    const theme::Scheme &s = c();
    ImFont *f = theme::fonts().body_strong;
    const float h = px(40);
    const float check_w = px(18) + px(6);

    float seg_w = 0.0f;
    for (int i = 0; i < count; i++) {
        seg_w = std::max(seg_w, text_size(f, items[i]).x + check_w + px(28));
    }
    float w = width > 0.0f ? width : seg_w * count;
    w = std::min(w, std::max(ImGui::GetContentRegionAvail().x, px(120)));
    seg_w = w / count;

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool changed = false;
    const float r = h * 0.5f;

    for (int i = 0; i < count; i++) {
        ImGui::PushID(i);
        const ImVec2 a(p.x + seg_w * i, p.y);
        const ImVec2 b(p.x + seg_w * (i + 1), p.y + h);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##seg", ImVec2(seg_w, h)) && *selected != i) {
            *selected = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const bool sel = (*selected == i);
        const float sv = theme::animate(ImGui::GetID("##s"), sel ? 1.0f : 0.0f, 0.2f);
        const float hv = theme::animate(ImGui::GetID("##h"), hovered ? 1.0f : 0.0f, 0.15f);

        ImDrawFlags corners = ImDrawFlags_RoundCornersNone;
        if (i == 0) corners = ImDrawFlags_RoundCornersLeft;
        if (i == count - 1) corners = (count == 1) ? ImDrawFlags_RoundCornersAll
                                                   : ImDrawFlags_RoundCornersRight;
        if (sv > 0.001f) {
            dl->AddRectFilled(a, b, u32(s.secondary_container, sv), r, corners);
        }
        if (hv > 0.001f) {
            dl->AddRectFilled(a, b, u32(s.on_surface, 0.08f * hv), r, corners);
        }

        const ImVec4 fg = theme::mix(s.on_surface, s.on_secondary_container, sv);
        const ImVec2 ts = text_size(f, items[i]);
        const float cw = check_w * sv;
        float x = a.x + (seg_w - (ts.x + cw)) * 0.5f;
        if (sv > 0.05f && theme::fonts().have_icons) {
            draw_icon(dl, ImVec2(x + px(9) * sv, p.y + h * 0.5f), theme::icon::check,
                      theme::with_alpha(fg, sv));
        }
        draw_text(dl, f, ImVec2(x + cw, p.y + (h - ts.y) * 0.5f), fg, items[i]);
        if (i > 0) {
            dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, b.y), u32(s.outline), px(1));
        }
        ImGui::PopID();
    }
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), u32(s.outline), r, 0, px(1));
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    return changed;
}

/* ── 开关 ─────────────────────────────────────────────────────────── */

bool toggle(const char *label, bool *value, float width)
{
    const theme::Scheme &s = c();
    ImGui::PushID(label);
    const float tw = px(52), th = px(32);
    const ImVec2 ls = text_size(ImGui::GetFont(), label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float total = width > 0 ? width : (ls.x > 0 ? ls.x + px(16) : 0) + tw;
    const bool clicked = ImGui::InvisibleButton("##sw", ImVec2(total, th));
    if (clicked) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float on = theme::animate(ImGui::GetID("##on"), *value ? 1.0f : 0.0f, 0.22f);
    const float hv = theme::animate(ImGui::GetID("##h"), hovered ? 1.0f : 0.0f, 0.15f);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (ls.x > 0) {
        draw_text(dl, ImGui::GetFont(), ImVec2(p.x, p.y + (th - ls.y) * 0.5f), s.on_surface,
                  label);
    }
    const ImVec2 a(p.x + total - tw, p.y);
    const ImVec2 b(a.x + tw, a.y + th);
    const float r = th * 0.5f;

    dl->AddRectFilled(a, b, u32(theme::mix(s.surface_container_highest, s.primary, on)), r);
    if (on < 0.999f) {
        dl->AddRect(a, b, u32(s.outline, 1.0f - on), r, 0, px(2));
    }

    float thumb = px(16) + (px(24) - px(16)) * on;
    if (active) thumb = px(28);
    const float cx = a.x + r + (tw - th) * on;
    const ImVec2 ctr(cx, a.y + r);
    if (hv > 0.001f) {
        dl->AddCircleFilled(ctr, px(20), u32(on > 0.5f ? s.primary : s.on_surface, 0.08f * hv), 32);
    }
    dl->AddCircleFilled(ctr, thumb * 0.5f,
                        u32(theme::mix(s.outline, s.on_primary, on)), 32);
    if (on > 0.5f && theme::fonts().have_icons) {
        ImGui::PushFont(theme::fonts().label);
        draw_icon(dl, ctr, theme::icon::check, theme::with_alpha(s.on_primary_container, on),
                  theme::fonts().label);
        ImGui::PopFont();
    }
    ImGui::PopID();
    return clicked;
}

/* ── Chip ─────────────────────────────────────────────────────────── */

bool filter_chip(const char *label, bool *on)
{
    const theme::Scheme &s = c();
    ImFont *f = theme::fonts().body_strong;
    const float h = px(32);
    ImGui::PushID(label);
    const float sv = theme::animate(ImGui::GetID("##s"), *on ? 1.0f : 0.0f, 0.2f);
    const ImVec2 ts = text_size(f, label);
    const float icon_w = (theme::fonts().have_icons ? px(18) + px(8) : 0.0f) * sv;
    const float w = px(16) + icon_w + ts.x + px(16);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##chip", ImVec2(w, h));
    if (clicked) *on = !*on;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float hv = theme::animate(ImGui::GetID("##h"), hovered ? 1.0f : 0.0f, 0.15f);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    const float r = px(8);
    if (sv > 0.001f) {
        dl->AddRectFilled(p, q, u32(s.secondary_container, sv), r);
    }
    if (sv < 0.999f) {
        dl->AddRect(p, q, u32(s.outline_variant, 1.0f - sv), r, 0, px(1));
    }
    const ImVec4 fg = theme::mix(s.on_surface_variant, s.on_secondary_container, sv);
    if (hv > 0.001f) dl->AddRectFilled(p, q, u32(fg, 0.08f * hv), r);
    if (sv > 0.05f && theme::fonts().have_icons) {
        draw_icon(dl, ImVec2(p.x + px(16) + px(9), p.y + h * 0.5f), theme::icon::check,
                  theme::with_alpha(fg, sv));
    }
    draw_text(dl, f, ImVec2(p.x + px(16) + icon_w, p.y + (h - ts.y) * 0.5f), fg, label);
    ImGui::PopID();
    return clicked;
}

void chip(const char *label, const ImVec4 &bg, const ImVec4 &fg, const char *icon)
{
    ImFont *f = theme::fonts().label;
    const float h = px(28);
    const ImVec2 ts = text_size(f, label);
    const float iw = (icon && theme::fonts().have_icons) ? px(16) + px(6) : 0.0f;
    const float w = px(12) + iw + ts.x + px(12);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(bg), px(8));
    if (iw > 0) {
        draw_icon(dl, ImVec2(p.x + px(12) + px(8), p.y + h * 0.5f), icon, fg, theme::fonts().label);
    }
    draw_text(dl, f, ImVec2(p.x + px(12) + iw, p.y + (h - ts.y) * 0.5f), fg, label);
}

/* ── 滑块 ─────────────────────────────────────────────────────────── */

static bool slider_impl(const char *label, float *v, float vmin, float vmax, const char *value_text,
                        float width, float step)
{
    const theme::Scheme &s = c();
    ImGui::PushID(label);
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;

    /* 第一行：标签 + 数值 */
    const ImVec2 row = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 vs = text_size(theme::fonts().body_strong, value_text, value_text + strlen(value_text));
    draw_text(dl, ImGui::GetFont(), row, s.on_surface, label);
    draw_text(dl, theme::fonts().body_strong, ImVec2(row.x + w - vs.x, row.y), s.primary,
              value_text, value_text + strlen(value_text));
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));

    const float h = px(28);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sl", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float hr = px(10);
    const float x0 = p.x + hr, x1 = p.x + w - hr;
    bool changed = false;
    if (active && x1 > x0) {
        float t = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0);
        t = std::clamp(t, 0.0f, 1.0f);
        float nv = vmin + (vmax - vmin) * t;
        if (step > 0) nv = vmin + std::round((nv - vmin) / step) * step;
        nv = std::clamp(nv, vmin, vmax);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }
    if (hovered && !active && ImGui::GetIO().MouseWheel != 0.0f) {
        const float d = step > 0 ? step : (vmax - vmin) / 100.0f;
        const float nv = std::clamp(*v + (ImGui::GetIO().MouseWheel > 0 ? d : -d), vmin, vmax);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }

    const float t = vmax > vmin ? (*v - vmin) / (vmax - vmin) : 0.0f;
    const float cy = p.y + h * 0.5f;
    const float hx = x0 + (x1 - x0) * t;
    const float th = px(4);
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f),
                      u32(s.secondary_container), th);
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(hx, cy + th * 0.5f), u32(s.primary), th);

    const float act = theme::animate(ImGui::GetID("##a"), active ? 1.0f : (hovered ? 0.5f : 0.0f), 0.15f);
    if (act > 0.01f) dl->AddCircleFilled(ImVec2(hx, cy), px(20), u32(s.primary, 0.10f * act), 32);
    dl->AddCircleFilled(ImVec2(hx, cy), hr, u32(s.primary), 32);

    /* 拖动时的数值气泡 */
    if (act > 0.6f && active) {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        const float bw = vs.x + px(16), bh = vs.y + px(8);
        const ImVec2 ba(hx - bw * 0.5f, p.y - bh - px(6));
        fg->AddRectFilled(ba, ImVec2(ba.x + bw, ba.y + bh), u32(s.inverse_surface), bh * 0.5f);
        draw_text(fg, theme::fonts().body_strong, ImVec2(ba.x + px(8), ba.y + px(4)),
                  s.inverse_on_surface, value_text, value_text + strlen(value_text));
    }
    ImGui::PopID();
    return changed;
}

bool slider_float(const char *label, float *v, float vmin, float vmax, const char *fmt, float width)
{
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, *v);
    return slider_impl(label, v, vmin, vmax, buf, width, 0.0f);
}

bool slider_int(const char *label, int *v, int vmin, int vmax, const char *fmt, float width)
{
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, *v);
    float f = static_cast<float>(*v);
    const bool ch = slider_impl(label, &f, static_cast<float>(vmin), static_cast<float>(vmax), buf,
                                width, 1.0f);
    if (ch) *v = static_cast<int>(std::lround(f));
    return ch;
}

/* ── 文本框 ───────────────────────────────────────────────────────── */

bool text_field(const char *id, const char *hint, char *buf, size_t cap, const char *icon,
                float width)
{
    const theme::Scheme &s = c();
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const float h = px(44);
    const float lead = (icon && theme::fonts().have_icons) ? px(44) : px(18);
    const float pad_y = (h - ImGui::GetFontSize()) * 0.5f;

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool focused_prev = theme::animate(ImGui::GetID("##f"), 0.0f, 0.0f) > 0.5f;
    (void)focused_prev;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(lead, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, s.surface_container_highest);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                          theme::state_layer(s.surface_container_highest, s.on_surface, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, s.surface_container_highest);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, s.on_surface_variant);
    ImGui::SetNextItemWidth(w);
    const bool changed = ImGui::InputTextWithHint("##tf", hint, buf, cap);
    const bool active = ImGui::IsItemActive();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);

    const float fa = theme::animate(ImGui::GetID("##fa"), active ? 1.0f : 0.0f, 0.18f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + ImGui::GetItemRectSize().y);
    if (fa > 0.01f) {
        dl->AddRect(p, q, u32(s.primary, fa), (q.y - p.y) * 0.5f, 0, px(2));
    }
    if (icon && theme::fonts().have_icons) {
        draw_icon(dl, ImVec2(p.x + px(22), (p.y + q.y) * 0.5f), icon,
                  theme::mix(s.on_surface_variant, s.primary, fa));
    }
    ImGui::PopID();
    return changed;
}

/* ── 卡片 ─────────────────────────────────────────────────────────── */

namespace {

struct CardState {
    float height = 0.0f;
};
std::unordered_map<ImGuiID, CardState> g_cards;

struct CardFrame {
    ImGuiID id;
    CardKind kind;
    ImVec2 pos;
    float width;
};
std::vector<CardFrame> g_card_stack;

} // namespace

float column_width(int columns)
{
    const float gap = px(16);
    const float avail = ImGui::GetContentRegionAvail().x;
    if (columns <= 1) return avail;
    return std::floor((avail - gap * (columns - 1)) / columns);
}

void same_line_card()
{
    ImGui::SameLine(0.0f, px(16));
}

void card_begin(const char *id, float width, CardKind kind, float min_height)
{
    const theme::Scheme &s = c();
    const ImGuiID gid = ImGui::GetID(id);
    CardState &st = g_cards[gid];
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = std::max(st.height, min_height);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    const float r = px(kind == CardKind::Hero ? 24.0f : 16.0f);

    if (h > 0.0f) {
        /* M3 用色调表达层级：Elevated = surfaceContainerLow，Filled = Highest。
         * 卡片本身不可点击，所以没有状态层。 */
        switch (kind) {
        case CardKind::Elevated:
            dl->AddRectFilled(p, q, u32(s.surface_container_low), r);
            break;
        case CardKind::Filled:
            dl->AddRectFilled(p, q, u32(s.surface_container_highest), r);
            break;
        case CardKind::Outlined:
            dl->AddRectFilled(p, q, u32(s.surface), r);
            dl->AddRect(p, q, u32(s.outline_variant), r, 0, px(1));
            break;
        case CardKind::Hero:
            dl->AddRectFilled(p, q, u32(s.primary_container), r);
            break;
        }
    }

    g_card_stack.push_back(CardFrame{gid, kind, p, w});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20), px(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(10), px(10)));
    if (kind == CardKind::Hero) {
        ImGui::PushStyleColor(ImGuiCol_Text, s.on_primary_container);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, s.on_surface);
    }
    ImGui::BeginChild(id, ImVec2(w, min_height),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
}

void card_end()
{
    if (g_card_stack.empty()) {
        return;
    }
    const CardFrame fr = g_card_stack.back();
    g_card_stack.pop_back();

    CardState &st = g_cards[fr.id];
    st.height = ImGui::GetWindowSize().y;
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

/* ── 数据展示 ─────────────────────────────────────────────────────── */

void stat(const char *label, const char *value, const char *unit, const ImVec4 *accent,
          const char *sub)
{
    const theme::Scheme &s = c();
    text_label(label);
    ImGui::PushFont(theme::fonts().headline);
    ImGui::PushStyleColor(ImGuiCol_Text, accent ? *accent : s.on_surface);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (unit && unit[0]) {
        ImGui::SameLine(0, px(6));
        /* 单位与大数字底部对齐 */
        const float dy = theme::fonts().headline->FontSize - ImGui::GetFontSize();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dy * 0.72f);
        ImGui::PushStyleColor(ImGuiCol_Text, s.on_surface_variant);
        ImGui::TextUnformatted(unit);
        ImGui::PopStyleColor();
    }
    if (sub && sub[0]) {
        text_label(sub);
    }
}

void progress(float frac, float width, const ImVec4 *color)
{
    const theme::Scheme &s = c();
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const float h = px(4);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, px(8)));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float cy = p.y + px(4);
    const ImVec4 col = color ? *color : s.primary;
    const ImVec2 a(p.x, cy - h * 0.5f), b(p.x + w, cy + h * 0.5f);
    dl->AddRectFilled(a, b, u32(s.secondary_container), h);
    if (frac < 0.0f) {
        /* 不确定进度：一段来回扫动的条 */
        const float t = static_cast<float>(std::fmod(theme::time() * 0.8, 1.0));
        const float seg = w * 0.3f;
        const float x = p.x - seg + (w + seg) * t;
        dl->PushClipRect(a, b, true);
        dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x + seg, b.y), u32(col), h);
        dl->PopClipRect();
        return;
    }
    frac = std::clamp(frac, 0.0f, 1.0f);
    if (frac > 0.0f) {
        const ImVec2 e(p.x + w * frac, b.y);
        dl->AddRectFilled(a, e, u32(col), h);
    }
}

void usage_bar(const char *label, double used, double total, const char *unit, const ImVec4 *color)
{
    const theme::Scheme &s = c();
    const float frac = total > 0 ? static_cast<float>(used / total) : 0.0f;
    char right[96];
    snprintf(right, sizeof(right), "%.1f / %.1f %s  (%.0f%%)", used, total, unit, frac * 100.0f);
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(label);
    const ImVec2 rs = ImGui::CalcTextSize(right);
    ImGui::SameLine(w - rs.x);
    ImGui::PushStyleColor(ImGuiCol_Text, s.on_surface_variant);
    ImGui::TextUnformatted(right);
    ImGui::PopStyleColor();
    ImVec4 col = color ? *color : s.primary;
    if (frac > 0.9f) col = s.error;
    else if (frac > 0.75f) col = s.warning;
    progress(frac, w, &col);
}

void line_chart(const char *id, const Series *series, int series_count, float vmin, float vmax,
                ImVec2 size, const char *unit)
{
    const theme::Scheme &s = c();
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##chart", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);

    /* 自动扩展上界，保证曲线不出框 */
    float top = vmax;
    for (int k = 0; k < series_count; k++) {
        for (int i = 0; i < series[k].count; i++) top = std::max(top, series[k].data[i] * 1.1f);
    }
    if (top <= vmin) top = vmin + 1.0f;
    /* 刻度取整：4 格、每格是 {1,1.5,2,2.5,3,4,5,6,8}×10^n，避免 "22 / 15 / 8" 这种标签 */
    float step = (top - vmin) / 4.0f;
    {
        const float p10 = std::pow(10.0f, std::floor(std::log10(step)));
        const float f = step / p10;
        float nf = 10.0f;
        for (float c : {1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f}) {
            if (f <= c * 1.0001f) {
                nf = c;
                break;
            }
        }
        step = nf * p10;
        top = vmin + step * 4.0f;
    }
    const bool int_ticks = std::fabs(step - std::round(step)) < 1e-3f;

    ImFont *lf = theme::fonts().label;
    const float label_w = px(44);
    const ImVec2 a(p.x + label_w, p.y + px(4));
    const ImVec2 b(q.x, q.y - px(4));
    const float gw = b.x - a.x, gh = b.y - a.y;

    for (int g = 0; g <= 4; g++) {
        const float y = a.y + gh * g / 4.0f;
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), u32(s.outline_variant, g == 4 ? 0.9f : 0.45f),
                    px(1));
        char buf[32];
        snprintf(buf, sizeof(buf), int_ticks ? "%.0f" : "%.1f", top - step * g);
        const ImVec2 ts = text_size(lf, buf);
        draw_text(dl, lf, ImVec2(a.x - px(8) - ts.x, y - ts.y * 0.5f), s.on_surface_variant, buf);
    }

    if (gw <= 1.0f || gh <= 1.0f) {
        ImGui::PopID();
        return;
    }

    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    static std::vector<ImVec2> pts; /* 只在 UI 线程用；复用避免每帧分配 */

    for (int k = 0; k < series_count; k++) {
        const Series &sr = series[k];
        if (sr.count < 2) continue;
        pts.resize(static_cast<size_t>(sr.count));
        for (int i = 0; i < sr.count; i++) {
            const float v = sr.data[(sr.offset + i) % sr.count];
            const float t = std::clamp((v - vmin) / (top - vmin), 0.0f, 1.0f);
            pts[static_cast<size_t>(i)] =
                ImVec2(a.x + gw * i / (sr.count - 1), b.y - gh * t);
        }
        /* 渐变填充 */
        const ImU32 ctop = u32(sr.color, 0.16f);
        const ImU32 cbot = u32(sr.color, 0.0f);
        dl->PrimReserve((sr.count - 1) * 6, (sr.count - 1) * 4);
        for (int i = 0; i + 1 < sr.count; i++) {
            const ImVec2 &p0 = pts[static_cast<size_t>(i)];
            const ImVec2 &p1 = pts[static_cast<size_t>(i + 1)];
            const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
            dl->PrimWriteVtx(p0, white, ctop);
            dl->PrimWriteVtx(p1, white, ctop);
            dl->PrimWriteVtx(ImVec2(p1.x, b.y), white, cbot);
            dl->PrimWriteVtx(ImVec2(p0.x, b.y), white, cbot);
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
        }
        dl->AddPolyline(pts.data(), sr.count, u32(sr.color), 0, px(2));
        const ImVec2 last = pts.back();
        dl->AddCircleFilled(last, px(3.5f), u32(sr.color), 16);
    }

    if (hovered && series_count > 0 && series[0].count > 1) {
        const float mx = ImGui::GetIO().MousePos.x;
        const int n = series[0].count;
        int i = static_cast<int>(std::lround((mx - a.x) / gw * (n - 1)));
        i = std::clamp(i, 0, n - 1);
        const float x = a.x + gw * i / (n - 1);
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), u32(s.on_surface_variant, 0.5f), px(1));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, s.inverse_surface);
        ImGui::PushStyleColor(ImGuiCol_Text, s.inverse_on_surface);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(10), px(6)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(6));
        ImGui::BeginTooltip();
        for (int k = 0; k < series_count; k++) {
            const Series &sr = series[k];
            if (i >= sr.count) continue;
            const float v = sr.data[(sr.offset + i) % sr.count];
            ImGui::TextColored(sr.color, "\xe2\x97\x8f");
            ImGui::SameLine();
            ImGui::Text("%s  %.1f %s", sr.name ? sr.name : "", v, unit ? unit : "");
        }
        ImGui::EndTooltip();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    /* 图例 */
    if (series_count > 1) {
        float lx = q.x;
        for (int k = series_count - 1; k >= 0; k--) {
            const char *nm = series[k].name ? series[k].name : "";
            const ImVec2 ts = text_size(lf, nm);
            lx -= ts.x;
            draw_text(dl, lf, ImVec2(lx, p.y), s.on_surface_variant, nm);
            lx -= px(12);
            dl->AddCircleFilled(ImVec2(lx + px(4), p.y + ts.y * 0.5f), px(4), u32(series[k].color), 12);
            lx -= px(14);
        }
    }
    ImGui::PopID();
}

void stacked_bar(const char *id, const Segment *segs, int count, float budget, float width,
                 const char *unit)
{
    const theme::Scheme &s = c();
    ImGui::PushID(id);
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const float h = px(28);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();

    float total = 0.0f;
    for (int i = 0; i < count; i++) total += std::max(0.0f, segs[i].value);
    const float scale_max = std::max(total, budget) * 1.12f;
    const float r = px(8);
    const ImVec2 q(p.x + w, p.y + h);
    dl->AddRectFilled(p, q, u32(s.surface_container_highest), r);

    float x = p.x;
    int last_nonzero = -1;
    for (int i = 0; i < count; i++) if (segs[i].value > 0) last_nonzero = i;
    bool first = true;
    for (int i = 0; i < count; i++) {
        if (segs[i].value <= 0 || scale_max <= 0) continue;
        const float sw = w * segs[i].value / scale_max;
        ImDrawFlags fl = ImDrawFlags_RoundCornersNone;
        if (first && i == last_nonzero) fl = ImDrawFlags_RoundCornersAll;
        else if (first) fl = ImDrawFlags_RoundCornersLeft;
        else if (i == last_nonzero) fl = ImDrawFlags_RoundCornersRight;
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + sw, q.y), u32(segs[i].color), r, fl);
        if (!first) dl->AddLine(ImVec2(x, p.y), ImVec2(x, q.y), u32(s.surface_container_low), px(2));
        first = false;
        x += sw;
    }
    if (budget > 0 && total > budget) {
        dl->AddRect(p, q, u32(s.error), r, 0, px(2)); /* 超预算：错误色描边 */
    }
    if (budget > 0 && scale_max > 0) {
        const float bx = p.x + w * budget / scale_max;
        for (float y = p.y - px(4); y < q.y + px(4); y += px(6)) {
            dl->AddLine(ImVec2(bx, y), ImVec2(bx, std::min(y + px(3), q.y + px(4))), u32(s.error),
                        px(2));
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "budget %.1f %s", budget, unit);
        ImFont *lf = theme::fonts().label;
        const ImVec2 ts = text_size(lf, buf);
        draw_text(dl, lf, ImVec2(std::min(bx + px(6), q.x - ts.x), q.y + px(6)), s.error, buf);
    }
    ImGui::Dummy(ImVec2(w, px(14)));

    /* 图例 */
    for (int i = 0; i < count; i++) {
        if (i > 0) ImGui::SameLine(0, px(18));
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        const float lh = ImGui::GetTextLineHeight();
        ImGui::Dummy(ImVec2(px(10), lh));
        dl->AddRectFilled(ImVec2(cp.x, cp.y + lh * 0.5f - px(5)), ImVec2(cp.x + px(10), cp.y + lh * 0.5f + px(5)),
                          u32(segs[i].color), px(3));
        ImGui::SameLine(0, px(6));
        ImGui::Text("%s", segs[i].name);
        ImGui::SameLine(0, px(4));
        ImGui::PushStyleColor(ImGuiCol_Text, s.on_surface_variant);
        ImGui::Text("%.2f %s", segs[i].value, unit);
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

bool kv_begin(const char *id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        return false;
    }
    ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}

void kv_row(const char *key, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::TableNextRow(0, px(30));
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, c().on_surface_variant);
    ImGui::TextUnformatted(key);
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(buf);
}

void kv_end()
{
    ImGui::EndTable();
}

void status_dot(const ImVec4 &col, float radius)
{
    const float r = px(radius);
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(r * 2.0f, lh));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 ctr(p.x + r, p.y + lh * 0.5f);
    dl->AddCircleFilled(ctr, r, u32(col), 16);
}

void empty_state(const char *icon, const char *title, const char *text)
{
    const theme::Scheme &s = c();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float block_h = px(240);
    const float y0 = ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - block_h) * 0.4f);
    ImGui::SetCursorPosY(y0);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float d = px(96);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ctr(p.x + avail.x * 0.5f, p.y + d * 0.5f);
    dl->AddCircleFilled(ctr, d * 0.5f, u32(s.secondary_container), 48);
    draw_icon(dl, ctr, icon, s.on_secondary_container, theme::fonts().icon_large);
    ImGui::Dummy(ImVec2(avail.x, d + px(18)));

    ImGui::PushFont(theme::fonts().title);
    const ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - ts.x) * 0.5f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();

    const float wrap = std::min(avail.x, px(460));
    const ImVec2 bs = ImGui::CalcTextSize(text, nullptr, false, wrap);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - bs.x) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, s.on_surface_variant);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

/* ── Snackbar ─────────────────────────────────────────────────────── */

namespace {

struct Snack {
    SnackKind kind;
    std::string text;
};

std::mutex g_snack_mu;
std::deque<Snack> g_snacks;
float g_snack_t = -1.0f; /* 当前条已显示时间，<0 = 无 */

constexpr float kSnackShow = 3.6f;

} // namespace

void snackbar(SnackKind kind, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_snack_mu);
    g_snacks.push_back({kind, buf});
    while (g_snacks.size() > 8) g_snacks.pop_front();
}

bool snackbar_active()
{
    std::lock_guard<std::mutex> lk(g_snack_mu);
    return !g_snacks.empty();
}

void snackbar_draw()
{
    Snack cur;
    {
        std::lock_guard<std::mutex> lk(g_snack_mu);
        if (g_snacks.empty()) {
            g_snack_t = -1.0f;
            return;
        }
        cur = g_snacks.front();
    }
    if (g_snack_t < 0.0f) g_snack_t = 0.0f;
    g_snack_t += frame_dt();

    const theme::Scheme &s = c();
    const float in = std::min(1.0f, g_snack_t / 0.22f);
    const float out = std::min(1.0f, std::max(0.0f, (kSnackShow - g_snack_t) / 0.2f));
    const float vis = theme::appearance().reduce_motion ? 1.0f : theme::ease_emphasized(std::min(in, out));

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImFont *f = ImGui::GetFont();
    const float maxw = std::min(vp->WorkSize.x - px(48), px(560));
    const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, maxw - px(80), cur.text.c_str());
    const float w = std::min(maxw, ts.x + px(16 + 24 + 12 + 16 + 40));
    const float h = std::max(px(48), ts.y + px(28));
    const ImVec2 p(vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f,
                   vp->WorkPos.y + vp->WorkSize.y - (h + px(24)) * vis);

    ImGui::SetNextWindowPos(p);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.0f, vis));
    ImGui::Begin("##snackbar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec4 accent = cur.kind == SnackKind::Error     ? s.error
                          : cur.kind == SnackKind::Success ? s.success
                                                           : s.inverse_primary;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(s.inverse_surface), px(4));
    const char *ic = cur.kind == SnackKind::Error     ? theme::icon::error
                     : cur.kind == SnackKind::Success ? theme::icon::check
                                                      : theme::icon::info;
    draw_icon(dl, ImVec2(p.x + px(28), p.y + h * 0.5f), ic, accent);
    dl->AddText(f, f->FontSize, ImVec2(p.x + px(52), p.y + (h - ts.y) * 0.5f),
                u32(s.inverse_on_surface), cur.text.c_str(), nullptr, maxw - px(80));

    /* 关闭按钮 */
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - px(44), p.y + (h - px(40)) * 0.5f));
    bool dismiss = ImGui::InvisibleButton("##x", ImVec2(px(40), px(40)));
    draw_icon(dl, ImVec2(p.x + w - px(24), p.y + h * 0.5f), theme::icon::cancel,
              s.inverse_on_surface);
    ImGui::End();
    ImGui::PopStyleVar(2);

    if (dismiss && g_snack_t < kSnackShow - 0.2f) {
        g_snack_t = kSnackShow - 0.2f;
    }
    if (g_snack_t >= kSnackShow) {
        std::lock_guard<std::mutex> lk(g_snack_mu);
        if (!g_snacks.empty()) g_snacks.pop_front();
        g_snack_t = -1.0f;
    }
}

/* ── 对话框 ───────────────────────────────────────────────────────── */

namespace {
std::set<std::string> g_dialog_requests;
}

void open_dialog(const char *id)
{
    g_dialog_requests.insert(id);
}

int dialog(const char *id, const char *icon, const char *title, const char *text,
           const char *confirm, bool danger)
{
    const theme::Scheme &s = c();
    auto it = g_dialog_requests.find(id);
    if (it != g_dialog_requests.end()) {
        ImGui::OpenPopup(id);
        g_dialog_requests.erase(it);
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(px(320), 0), ImVec2(px(560), FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(24)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(28));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    int result = 0;
    if (ImGui::BeginPopupModal(id, nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), u32(s.surface_container_high),
                          px(28));

        const float inner = std::max(ImGui::GetContentRegionAvail().x, px(312));
        if (icon && theme::fonts().have_icons) {
            ImGui::PushFont(theme::fonts().icon);
            const ImVec2 is = ImGui::CalcTextSize(icon);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - is.x) * 0.5f);
            ImGui::TextColored(danger ? s.error : s.secondary, "%s", icon);
            ImGui::PopFont();
        }
        ImGui::PushFont(theme::fonts().title);
        const ImVec2 tsz = ImGui::CalcTextSize(title);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (inner - tsz.x) * 0.5f));
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, px(4)));
        ImGui::PushStyleColor(ImGuiCol_Text, s.on_surface_variant);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + px(420));
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, px(12)));

        /* 按钮右对齐：Cancel + Confirm */
        const float bw_cancel = text_size(theme::fonts().body_strong, "Cancel").x + px(24);
        const float bw_ok = text_size(theme::fonts().body_strong, confirm).x + px(48);
        const float total = bw_cancel + px(8) + bw_ok;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, inner - total));
        if (button("Cancel", ButtonKind::Text, nullptr, bw_cancel) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            result = -1;
        }
        ImGui::SameLine(0, px(8));
        if (button(confirm, danger ? ButtonKind::Danger : ButtonKind::Filled, nullptr, bw_ok)) {
            result = 1;
        }
        if (result != 0) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return result;
}

void tooltip(const char *text)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        return;
    }
    const theme::Scheme &s = c();
    ImGui::PushStyleColor(ImGuiCol_PopupBg, s.inverse_surface);
    ImGui::PushStyleColor(ImGuiCol_Text, s.inverse_on_surface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(4));
    ImGui::PushFont(theme::fonts().label);
    ImGui::SetTooltip("%s", text);
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

} // namespace ui
