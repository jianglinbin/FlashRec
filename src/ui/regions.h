#pragma once
// UI 几何唯一真值（区域重绘的归因基础）。v0.4.0 / REGION_REDRAW_PLAN.md §5。
//
// 职责：把「可绘制元素的外接矩形」集中到一份实现，绘制 / 命中（bar_hit）/
// 悬停裁决（hover_key_of）/ 脏区归因（render_loop）全部同源 —— 延续
// "几何禁止两处各算" 的既有铁律（d24 起的底栏几何同源规则的全量版）。
// 底栏细几何 geom_of 自 chrome.cpp 提升（v0.4.0 d56），chrome 侧同样消费本文件。
//
// P2 待填：osd / thumb_card / ctx_menu / badges 的精确矩形（涉及各自绘制实现
// 内的自适应尺寸），P0/P1 归因不消费这些字段；ctx_menu 直接用调用方手里的
// CtxMenuLayout（x/y/w/h 即矩形），不在此重复展开。
#include <cstdint>

#include "ui/damage.h"
#include "ui/views/chrome.h"  // kBtnIdCount / ViewInput

struct NVGcontext;
typedef struct NVGcontext NVGcontext;

namespace fr {

struct Theme;
struct PlaybackSnapshot;

// 底栏细几何（自 chrome.cpp 匿名空间提升为共享；命中与绘制共用，禁两处各算）
struct Geom {
  float botY = 0, cy = 0;
  float btnPlayX = 0, btnPrevX = 0, btnNextX = 0;
  float btnStopX = 0;  // d158：停止键中心（= prev/next 两键中点；无上/下一集数据时使用）
  float timeCurX = 0, timeLeftX = 0;
  float trackX = 0, trackW = 0;
  float volIconX = 0, volX = 0, volW = 0;
  float speedX = 0;  // d169：倍速按钮中心（位于倒计时时间之后、静音键之前）
  float pipX = 0, fsX = 0;
};

Geom geom_of(NVGcontext* vg, const Theme& t, float w, float h,
             const PlaybackSnapshot& snap, bool media_enabled);

// —— 缩略图浮层布局（v0.4.0 d59 自 chrome.cpp 提升为共享；绘制与归因同源）——
// p = 进度百分比（0..1）；kb_mode = 键盘预览（面板隐藏，锚常驻迷你线）。
// 公式与 chrome.cpp 绘制段原样一致（translateX(-50%) + 屏内夹停），禁改动。
struct ThumbLayout {
  bool visible = false;              // 调用方的可见性判定独立于本结构（几何恒可算）
  float tx = 0, ty = 0;              // 卡左上（窗口逻辑坐标）
  float w = 0, h = 0;                // 卡尺寸
  float bubble_x = 0, bubble_w = 0;  // 下方时间气泡
  float anchor_px = 0;               // 进度锚点 x（气泡文字中线）
};
ThumbLayout thumb_layout_of(NVGcontext* vg, const Theme& t, float w, float h, const Geom& g,
                            const PlaybackSnapshot& snap, float p, bool kb_mode);

// 归因包络：浮层 + 时间气泡的整体外接带（横向全宽：气泡 x 随锚点移动）。
// 与 chrome.cpp 绘制段 dmg_hit 包络同式（禁两处各算）。
DmgRect thumb_band_of(const Theme& t, float w, float ty);

// —— d74 剪贴板链接提示条布局（regions 唯一真值：绘制/命中/归因/悬停同源）——
// 生命周期 = [at, at+hold+fade)，横幅顶部居中（全屏贴顶、窗口态在顶栏下方）。
// URL 文本宽度自适应（上限 promptMaxW，超宽由绘制侧截断；命中只关心横幅矩形）。
struct PromptLayout {
  bool visible = false;
  float x = 0, y = 0, w = 0, h = 0;        // 横幅矩形
  float btn_play_x = 0, btn_ignore_x = 0;  // 两按钮左缘（y/h 同按钮高）
  float btn_y = 0, btn_w = 0, btn_h = 0;
  float url_x = 0, url_max_w = 0;          // URL 文本起点与可用宽
};
PromptLayout prompt_layout_of(NVGcontext* vg, const Theme& t, float w, float h,
                              bool fullscreen, double now, double at, const char* url,
                              bool enabled);

struct UiRegions {
  // —— 带级（P0/P1 归因粒度）——
  DmgRect topbar;       // 顶栏整带（全屏不渲染，矩形仍给出，归因按 fullscreen 门控）
  DmgRect bottom_bar;   // 底栏整带
  DmgRect mini_line;    // 常驻迷你进度线带
  DmgRect center_play;  // 中央大播放键外接方（可见性见 center_visible）
  DmgRect badges;       // 诊断徽章列包络（render_loop 侧填，文本在 render_loop 手里）
  DmgRect osd;          // OSD 包络带（与 draw_osd 的 dmg_hit 包络同式：横全宽）
  DmgRect thumb_card;   // 缩略图浮层 + 时间气泡包络带（横全宽，y 随锚点走）
  DmgRect ctx_menu;     // 右键菜单面板（render_loop 出帧路径用 ctx_lay 补填）
  DmgRect speed_menu;   // d169 倍速档位弹层（render_loop 出帧路径用 speed_lay 补填）
  DmgRect skin_menu;    // d182 皮肤弹层（render_loop 出帧路径用 skin_lay 补填）
  DmgRect prompt_bar;   // d74 剪贴板链接提示条横幅
  // —— 控件级（P1/P3 归因粒度）——
  DmgRect win_btn[3];             // 顶栏三键热区
  DmgRect skin_btn;               // d182：顶栏皮肤按钮热区
  DmgRect bar_btn[kBtnIdCount];   // 底栏按钮热区（媒体类无会话时仍给几何，归因门控）
  DmgRect track, time_cur, time_left;
  DmgRect volume;                 // 音量条 + 滑块包络（热区同式 bar_hit.vol）
  // —— 状态位（归因门控用；compute_regions 填）——
  bool has_picture = false, media = false, fullscreen = false, pip = false;
  bool center_visible = false;    // 中央大播放键本帧是否绘制（暂停态 && 非画中画）
  bool osd_visible = false;       // OSD 本帧是否绘制（osd_kind!=0 && 非画中画）
  bool thumb_visible = false;     // 缩略图浮层本帧是否绘制（悬停/拖拽/键盘预览 && 有画面）
  bool prompt_visible = false;    // d74 提示条本帧是否绘制（clip_url 非空 && 生命周期内）
};

// 一次算出全部可绘制元素的矩形（每帧出帧路径调用一次；vg 仅用于时间文字测量，
// 纯 fontstash 操作无 GL，跳帧路径调用同样安全）。
// fade：两栏可见度（chrome_fade，缩略图浮层 kb_mode 判定用；pip/待机传 1）。
// thumb_enabled：缩略图预览源就绪（render_loop 的 thumb_ != nullptr）。
UiRegions compute_regions(NVGcontext* vg, const Theme& t, float w, float h,
                          const PlaybackSnapshot& snap, const ViewInput& in,
                          bool media, bool pip, float fade, bool thumb_enabled);

}  // namespace fr
