#pragma once
// 两态共用 chrome（顶栏右上角三键 + 底栏）。**必须只有一份实现** ——
// 播放态与待机态各写一份、各自补交互，正是"同一个按钮两处行为不一致"的根因。
//
// 可见性与可用性是两个维度：
//   - 可见性（画不画）由调用方决定：播放态两栏随不操作自动隐去、全屏时顶栏不画；待机态常显。
//   - 可用性 media_enabled（= 有媒体会话）：播放·暂停 / 上下集 / 进度条 只在有会话时可用；
//     窗口类控件（音量 / 画中画 / 全屏）任何状态全量可用。
#include <nanovg.h>

#include "ui/damage.h"  // DmgRect（draw_* 的 clip 参数，v0.4.0 d56）

namespace fr {

struct Theme;
struct PlaybackSnapshot;
struct ViewInput;
struct ViewCallbacks;

// 按钮 id（ButtonFx 数组下标；两态共用同一命名空间，禁止重复占用）
enum BtnId : int {
  kBtnPlay = 0,
  kBtnPrev,
  kBtnNext,
  kBtnMute,        // 音量图标 = 静音键
  kBtnPip,
  kBtnFullscreen,
  kBtnWinMin,
  kBtnWinMax,
  kBtnWinClose,
  kBtnCenterPlay,
  kBtnClipPlay,     // d74：剪贴板提示条「播放」按钮（横幅隐藏时 d57 冻结值清理）
  kBtnClipIgnore,   // d74：剪贴板提示条「忽略」按钮
  kBtnIdCount
};

// 底栏是否被下方控件占用（仅一个"这位置归不归 chrome"的判定来源）。
// consumed 比 any() 宽：它还要覆盖"媒体禁用位"与条上任意位置。
// 空 media 会话下媒体类控件不算占用，见实现说明。
struct BarHit {
  bool play = false, prev = false, next = false;
  bool track = false;   // 进度条条身（不含时间文字）
  bool time = false;    // 时间文字区
  bool mute = false, vol = false;
  bool pip = false, fs = false;
  // 进度条几何（绘制与渲染线程算悬停百分比共用同一份，禁止两处各算）
  float track_x = 0, track_w = 0;
  // 点击是否已被底栏消费（= 上面任一位，或与媒体无关的栏内位置）
  bool consumed = false;
  bool any() const { return play || prev || next || time || track || mute || vol || pip || fs; }
};

// 底栏控件命中查询（几何唯一来源：绘制、命中、可见度、空白处判定全部走这里）。
// vg 仅用于量时间文字宽度（几何里时间占位宽与实际文字同源）；可传帧内任意时刻的上下文。
BarHit bar_hit(NVGcontext* vg, const Theme& t, float w, float h, const PlaybackSnapshot& snap,
               const ViewInput& in, bool media_enabled);

// 中央大播放键半径（暂停时才有实体，但热区几何独立于状态 —— 空白处判定必须能在
// 绘制之前问出"这一点是否落在中央键上"，否则点中央键会同时被当空白处单击处理）。
float center_play_radius(const Theme& t);

// 两栏可见度（0..1）：静置 idleHideSec 后淡出；悬停底栏任意控件 / 拖拽中保持可见
float chrome_fade(NVGcontext* vg, const Theme& t, float w, float h, const PlaybackSnapshot& snap,
                  const ViewInput& in, bool media_enabled);

// 右上角三键（热区各 winBtnW × topBarH，无缝排列）
void draw_win_buttons(NVGcontext* vg, const Theme& t, float w, ViewInput& in, ViewCallbacks& cb,
                      const DmgRect* clip = nullptr);

// 顶栏是否被三键占用（拖动区判定 / 空白处判定用）。
// 定义在此：只有一行矩形判定，无需单独 .cpp；Theme 完整定义由调用方 include 保证。
inline bool topbar_hit(const Theme& t, float w, float mx, float my);

// 底栏（贴边半透明）。fade 由调用方给出：播放态传 chrome_fade()，待机态传 1。
// show_thumb_preview：悬停进度条时是否画缩略图浮层（仅播放态有画面时才有意义）。
// thumb_img：悬停点画面（nanovg 图像句柄，0 = 还没出图，只画占位框）。
// rad = 窗口圆角（0 = 全屏/最大化直角）：底栏只有下侧两角圆，圆角外不落地。
void draw_bottom_bar(NVGcontext* vg, const Theme& t, float w, float h,
                     const PlaybackSnapshot& snap, ViewInput& in, ViewCallbacks& cb,
                     bool media_enabled, float fade, bool show_thumb_preview,
                     float rad = 0.f, int thumb_img = 0,
                     const DmgRect* clip = nullptr);

// 常驻迷你进度线（d37）：贴底全宽 2px 细线，alpha = (1-fade)×max —— 面板隐去后
// 不消失，键盘拖动时兼作预览线（kb_progress_value 驱动）。无会话不画。
void draw_mini_progress(NVGcontext* vg, const Theme& t, float w, float h,
                        const PlaybackSnapshot& snap, const ViewInput& in, float fade,
                        const DmgRect* clip = nullptr);

// OSD 浮层（d28）：音量/进度反馈，底部中央；alpha 曲线由 in.now - in.osd_at 推算。
// 调用方 = player_view / empty_state（画中画不调）。与 chrome 淡出无关（两栏隐去也显示）。
void draw_osd(NVGcontext* vg, const Theme& t, float w, float h, const PlaybackSnapshot& snap,
              const ViewInput& in, const DmgRect* clip = nullptr);

// —— 右键上下文菜单（d45）——
// 独立于 ButtonFx/BtnId：菜单条目命中用自管下标，不占用按钮槽
//（kMax=16 已被 10 个 BtnId 占用，菜单再挂进去就爆了）。
enum class CtxAction {
  None, PlayPause, Prev, Next, ToggleFullscreen, Pip,
  // d146 R5：三项信息徽章合并为一项；R2：新增投屏自动全屏（两项均勾选 + R1 持久化）。
  // 旧 ShowInfo/ShowVideoFps/ShowUiFps 已废弃（台账记"已废弃"，编号不删）。
  ShowMediaInfo, CastAutoFullscreen, Close,
};

struct CtxMenuItem {
  const char* label = nullptr;  // nullptr = 分隔线（其余字段忽略）
  CtxAction action = CtxAction::None;
  bool enabled = true;
  bool checkable = false;       // 左侧留 ✓ 勾选位
  bool checked = false;
};

// 菜单几何（绘制与条目命中共用同一份，禁止两处各算）。
// ys[] 为条目在面板内的相对 y；面板位置由弹出锚点（右键按下点）+ 越界回夹决定。
struct CtxMenuLayout {
  float x = 0, y = 0, w = 0, h = 0;   // 面板矩形（窗口坐标）
  float item_h = 0, sep_h = 0, pad_y = 0;
  float ys[16] = {};
  bool sep[16] = {};
  int count = 0;
  int item_at(float px, float py) const;  // 命中条目下标（-1 = 无，含面板外）
};

// 光标处弹出（mx/my = 弹出锚点），越界回夹进窗口。vg 仅用于量条目文字宽。
CtxMenuLayout ctx_menu_layout(NVGcontext* vg, const Theme& t, float win_w, float win_h,
                              const CtxMenuItem* items, int n, float mx, float my);

// hover = 本帧悬停条目下标（调用方用 item_at 算好传入；-1 = 无）
void draw_ctx_menu(NVGcontext* vg, const Theme& t, const CtxMenuLayout& lay,
                   const CtxMenuItem* items, int n, int hover,
                   const DmgRect* clip = nullptr);

}  // namespace fr

// inline 实现必须在 Theme 完整定义可见之后（本文件被 include 时 Theme 已可见）
#include "ui/theme.h"

namespace fr {
inline bool topbar_hit(const Theme& t, float w, float mx, float my) {
  return my >= 0.f && my < t.layout.topBarH && mx >= w - 3.f * t.layout.winBtnW;
}
inline bool center_play_hit(const Theme& t, float w, float h, float mx, float my) {
  const float r = center_play_radius(t) * 0.5f;
  const float dx = mx - w * 0.5f, dy = my - h * 0.5f;
  return dx * dx + dy * dy <= r * r;
}
}  // namespace fr