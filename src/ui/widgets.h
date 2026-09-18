#pragma once
// 通用绘制件：栏 / 文本 / 进度条 / 音量条 / 图标 / 按钮交互。全部只吃 Theme token（规则 6）。
// 进度条铁律（设计稿对齐）：所有子元素水平锚定同一百分比，垂直恒为条中线 cy，禁止手算偏移。
#include <cstdint>

#include <nanovg.h>

#include "ui/damage.h"  // DmgRect（区域重绘裁剪通道）

namespace fr {

struct Theme;

// —— 帧封装 ——
void begin_frame(NVGcontext* vg, int w, int h);
void end_frame(NVGcontext* vg);

// —— 区域重绘：脏区裁剪通道（v0.4.0 d56；归因 P1 起，通道先就位）——
// clip=nullptr 表示"全量绘制"，恒命中。带级早退只准用在**纯绘制**段
//（无 button_hit/回调），含交互语义的段禁止早退 —— 交互必须每帧全跑。
bool dmg_hit(const DmgRect* clip, float x, float y, float w, float h);
// 开启 GL scissor（物理像素，y 翻转经 gl_scissor_y 统一换算）+ nanovg scissor
//（逻辑坐标）。nanovg 全程不调 glScissor（已核），二者无状态冲突。
// 必须与 dmg_scissor_end 成对调用。
void dmg_scissor_begin(NVGcontext* vg, int win_h, const DmgRect& r);
void dmg_scissor_end(NVGcontext* vg);

// —— 基元 ——
// 栏底色 + 上/下描边。rad = 窗口圆角：贴边栏只有"外侧两角"是圆角，
// 圆角外的像素不落地，透明帧缓冲才能透出桌面（四角不能出现不透明块）。
// bg 传 nullptr 用 t.barBg（底栏半透明）；顶栏传 &t.topBarBg（不透明）。
void bar(NVGcontext* vg, const Theme& t, float x, float y, float w, float h, bool top,
         float rad = 0.f, const NVGcolor* bg = nullptr);
// 整屏/区域底色（圆角外不落地）。rad <= 0.5 时退化为直角矩形。
void panel_bg(NVGcontext* vg, float x, float y, float w, float h, float rad, NVGcolor c);
// 圆角矩形填充（基元；进度条 / 徽章 / 按钮底色等通用）
void rounded_rect(NVGcontext* vg, float x, float y, float w, float h, float r, NVGcolor fill);
// —— 窗口边界裁剪 ——
// 把后续绘制与给定矩形求交。栏身（bar）本身用逐角圆角不会溢出，但子元素
// （按钮悬停底色等）可能压到窗口边缘 —— 窗口模式下会盖住圆角露出方角。
// 调用方自己算好矩形（如底栏按钮区 = 0, botY, win_w, win_h - botY）。
// 注意：**不要**用"四边内缩 pad"的形态表达"只裁底栏"，那会算出负高度把
// 整条底栏连按钮一起裁掉（d17 踩过）。空矩形（w/h <= 0）不设裁剪。
// 必须与 win_clip_reset 成对调用。
void win_clip_rect(NVGcontext* vg, float x, float y, float w, float h);
void win_clip_reset(NVGcontext* vg);

void text(NVGcontext* vg, const Theme& t, float x, float y, float size, NVGcolor color,
          const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
void text_truncated(NVGcontext* vg, const Theme& t, float x, float y, float size,
                    NVGcolor color, const char* str, float max_w);  // 超宽打省略号
float text_width(NVGcontext* vg, float size, const char* str);
void circle_btn(NVGcontext* vg, const Theme& t, float cx, float cy, float d,
                NVGcolor bg, NVGcolor border, NVGcolor icon);

// —— 按钮交互（立即模式下的唯一按钮状态；跨帧由调用方持有）——
// 触发模型：必须在按钮热区内按下、并在**同一个按钮上**释放，才算一次 clicked。
// 按下瞬间锁定归属按钮（press_id），光标拖出热区或本来按在别处 → 不触发。
// 这样拖进度条 / 音量条滑过其它按钮不会误触（旧实现用「悬停 && 按住」判断，有此缺陷）。
struct ButtonFx {
  static constexpr int kMax = 16;
  float hover[kMax] = {};
  float press[kMax] = {};
  int press_id = -1;      // 本次按下锁定的按钮 id（-1 = 无归属）
  bool was_down = false;  // 上一帧左键状态
};

struct BtnState {
  float hover = 0.f;        // 0..1 悬停淡入量（禁用态上限 0.5：给弱化反馈但不亮起）
  float press = 0.f;        // 0..1 按压量
  bool enabled = true;
  bool clicked = false;     // 本帧完成一次点击
};

// 每帧、每个按钮调用一次（id 唯一且 < ButtonFx::kMax）
BtnState button_hit(ButtonFx& fx, int id, bool inside, bool enabled, bool mouse_down, float dt,
                    const Theme& t);
// 每帧绘制收尾调用一次（推进"上一帧鼠标状态"）；画中画等无按钮帧也必须调用
void button_fx_end_frame(ButtonFx& fx, bool mouse_down);
// d54：按钮缓动是否未收敛（任一 hover/press 处于向目标过渡的中间态）。
// approach() 到位会精确吸附到定值（hover ∈ {0, 0.5, 1}、press ∈ {0, 1}），
// 故凡不等于定值即动画途中 —— 渲染侧用它把「缓动未完成」当成出帧理由，
// 动画被驱动到完成才静默（否则悬停底色冻在半程等 2s 心跳补全）。
// 禁用态悬停收敛值恰为 0.5，不会被误判成"卡在半程"。
bool buttons_busy(const ButtonFx& fx);
// d59：同判定的位掩码版 —— bit i = 按钮 i 仍在缓动（i = BtnId 枚举值）。
// 区域重绘归因侧据此把缓动动画精确到对应按钮矩形，不再整窗。
// buttons_busy(fx) 等价于 buttons_busy_mask(fx) != 0。
uint32_t buttons_busy_mask(const ButtonFx& fx);

// 按钮底座：悬停淡入**圆角矩形**底色（比自己的热区略小一圈，视觉上是一块可点区域）
// + 按压缩小；返回图标缩放系数（按压缩小，常态 1）。
float round_btn_bg(NVGcontext* vg, const Theme& t, float cx, float cy, float d,
                   const BtnState& st);
// 同上，但尺寸由调用方给（中央大播放键用 d/box，底栏按钮用热区/底色）
float btn_bg(NVGcontext* vg, const Theme& t, float cx, float cy, float d, float box,
             const BtnState& st);
// 图标色：禁用 → iconDisabled；否则 iconDim → icon 按悬停量插值
NVGcolor btn_icon_color(const Theme& t, const BtnState& st);

// —— 进度条（cy = 中线；percent ∈ [0,1]，buffer 可为 -1 表示无缓冲数据）——
struct ProgressHit {
  bool on_track = false;   // 悬停在条热区（可拖动）
  bool on_knob = false;
};
void progress_track(NVGcontext* vg, const Theme& t, float x, float cy, float w, float percent,
                    float buffer_percent, bool hovered_or_drag);
float progress_percent_at(float x, float track_x, float track_w);  // hit → percent
void chapter_ticks(NVGcontext* vg, const Theme& t, float x, float cy, float w,
                   const float* percents, int n);

// —— 音量条（cy = 中线；percent ∈ [0,1]）——
// knob_r > 0 时绘制滑块；muted = 条与滑块整体弱化（静音态，视觉上与"音量 0"区分开）
void volume_bar(NVGcontext* vg, const Theme& t, float cx, float cy, float w, float percent,
                float knob_r, bool muted);

// —— 图标（中心点定位；尺寸按 Theme 布局常量）——
void icon_play(NVGcontext* vg, float cx, float cy, float h, NVGcolor c);
void icon_pause(NVGcontext* vg, float cx, float cy, float h, NVGcolor c);
void icon_prev(NVGcontext* vg, float cx, float cy, float w, NVGcolor c);
void icon_next(NVGcontext* vg, float cx, float cy, float w, NVGcolor c);
void icon_volume(NVGcontext* vg, float cx, float cy, float w, NVGcolor c);
// 静音：喇叭 + 右下斜线（斜线为独立 path，调用方可按需拆开）
void icon_volume_muted(NVGcontext* vg, float cx, float cy, float w, NVGcolor c,
                       NVGcolor slash);
void icon_pip(NVGcontext* vg, float cx, float cy, float w, NVGcolor c);
void icon_fullscreen(NVGcontext* vg, float cx, float cy, float w, NVGcolor c);
// Windows 三键（右上角方形热区；hover / press 底色 + 图标随按压轻微缩小）
enum class WinBtn { Min, Max, Close };
void win_button(NVGcontext* vg, const Theme& t, float x, float y, float w, float h, WinBtn kind,
                const BtnState& st, bool active, float rad = 0.f, float win_w = 0.f);  // active: Max 显示"还原"图标

}  // namespace fr
