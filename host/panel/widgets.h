/*
 * widgets.h — Material 3 组件（基于 Dear ImGui 自绘）
 */

#pragma once

#include <string>

#include "imgui.h"
#include "theme.h"

namespace ui {

/* ── 文本 ─────────────────────────────────────────────────────────── */

void text_headline(const char *s);
void text_title(const char *s);
void text_label(const char *s);           /* label-large，onSurfaceVariant */
void text_body_variant(const char *fmt, ...);
void text_colored(const ImVec4 &c, const char *fmt, ...);
/** 分节标题（带上间距） */
void section(const char *s);

/* ── 按钮 ─────────────────────────────────────────────────────────── */

enum class ButtonKind { Filled, Tonal, Outlined, Text, Danger };

/**
 * M3 按钮：20px 全圆角，hover / press 状态层（8% / 12%）。
 * icon 可为 nullptr。width 0 = 自适应。
 */
bool button(const char *label, ButtonKind kind = ButtonKind::Filled, const char *icon = nullptr,
            float width = 0.0f, bool enabled = true);

/** 圆形图标按钮（40px） */
bool icon_button(const char *id, const char *icon, const char *tooltip = nullptr,
                 bool selected = false);

/* ── 选择类 ───────────────────────────────────────────────────────── */

/** 分段按钮，返回选择是否改变。 */
bool segmented(const char *id, const char *const *items, int count, int *selected,
               float width = 0.0f);

/** M3 开关。label 显示在左侧，开关贴右对齐到 width（0 = 紧随标签）。 */
bool toggle(const char *label, bool *value, float width = 0.0f);

/** 过滤 chip（可切换） */
bool filter_chip(const char *label, bool *on);

/** 只读 assist chip（能力标签等） */
void chip(const char *label, const ImVec4 &bg, const ImVec4 &fg, const char *icon = nullptr);

/** 滑块，带值气泡。返回值是否改变。 */
bool slider_float(const char *label, float *v, float vmin, float vmax, const char *fmt,
                  float width = 0.0f);
bool slider_int(const char *label, int *v, int vmin, int vmax, const char *fmt,
                float width = 0.0f);

/** 外框文本框（搜索等），带前置图标。 */
bool text_field(const char *id, const char *hint, char *buf, size_t cap, const char *icon,
                float width = 0.0f);

/* ── 容器 ─────────────────────────────────────────────────────────── */

enum class CardKind { Elevated, Filled, Outlined, Hero };

/**
 * 卡片。内容在 begin/end 之间正常布局；背景在 end 时补画在内容之下
 * （用独立的 ImDrawListSplitter，可以嵌套表格）。
 * width 0 = 撑满可用宽度；min_height 可选。层级用 M3 的 surface-container 色调表达。
 */
void card_begin(const char *id, float width = 0.0f, CardKind kind = CardKind::Elevated,
                float min_height = 0.0f);
void card_end();

/** 一排卡片之间：下一张卡片接在同一行。 */
void same_line_card();

/** 按列数均分可用宽度后每张卡的宽度 */
float column_width(int columns);

/* ── 数据展示 ─────────────────────────────────────────────────────── */

/** 大号数值磁贴（卡片里用）：标签、数值、单位、可选副标题。 */
void stat(const char *label, const char *value, const char *unit = nullptr,
          const ImVec4 *accent = nullptr, const char *sub = nullptr);

/** 线性进度条（M3 linear progress，4px 高，圆角）。frac < 0 表示不确定进度。 */
void progress(float frac, float width = 0.0f, const ImVec4 *color = nullptr);

/** 用量条：标题 + 已用/总量 + 进度条 */
void usage_bar(const char *label, double used, double total, const char *unit,
               const ImVec4 *color = nullptr);

struct Series {
    const float *data;
    int count;
    int offset; /* 环形缓冲的起点 */
    ImVec4 color;
    const char *name;
};

/** 折线图：网格、淡填充、折线、悬停读数。 */
void line_chart(const char *id, const Series *series, int series_count, float vmin, float vmax,
                ImVec2 size, const char *unit = "");

struct Segment {
    float value;
    ImVec4 color;
    const char *name;
};

/** 堆叠条（帧耗时构成），budget > 0 时画预算线。 */
void stacked_bar(const char *id, const Segment *segs, int count, float budget, float width,
                 const char *unit);

/** 键值表格的一行 */
void kv_row(const char *key, const char *fmt, ...);
bool kv_begin(const char *id);
void kv_end();

/** 状态点 */
void status_dot(const ImVec4 &color, float radius = 5.0f);

/** 空状态：大图标 + 标题 + 说明 */
void empty_state(const char *icon, const char *title, const char *text);

/* ── 反馈 ─────────────────────────────────────────────────────────── */

enum class SnackKind { Info, Success, Error };

/** 线程安全：可在任何线程调用 */
void snackbar(SnackKind kind, const char *fmt, ...);
/** 每帧在根窗口末尾调用 */
void snackbar_draw();

/**
 * 确认对话框。调用 open_dialog(id) 打开；每帧调用 dialog() 渲染，
 * 返回 1 = 确认，-1 = 取消，0 = 仍打开或未打开。
 */
void open_dialog(const char *id);
int dialog(const char *id, const char *icon, const char *title, const char *text,
           const char *confirm, bool danger = false);

/** 悬停提示（M3 plain tooltip） */
void tooltip(const char *text);

} // namespace ui
