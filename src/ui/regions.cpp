#include "ui/regions.h"

#include <algorithm>
#include <string>

#include <nanovg.h>

#include "app/event_bus.h"
#include "app/str_util.h"
#include "ui/theme.h"
#include "ui/views/player_view.h"  // ViewInput 完整定义（regions.h 只有前向声明）
#include "ui/widgets.h"

namespace fr {

namespace {

// float 几何 → int 逻辑像素（脏区外扩 1–2px 的责任在归因侧 add，见规划 §11）
inline int dm(float v) { return (int)v; }

}  // namespace

// —— 底栏细几何（自 chrome.cpp 原样提升，公式不得改动；命中/绘制/归因同源）——
Geom geom_of(NVGcontext* vg, const Theme& t, float w, float h, const PlaybackSnapshot& snap,
             bool media_enabled) {
  const auto& L = t.layout;
  Geom g;
  g.botY = h - L.botBarH;
  g.cy = g.botY + L.botBarH * 0.5f;

  // 无媒体：时间用等宽占位，布局尺寸不随内容跳动
  const std::string cur =
      media_enabled ? format_hms_seconds(snap.position) : std::string("--:--");
  const std::string left =
      media_enabled ? "-" + format_hms_seconds(std::max(0.0, snap.duration - snap.position))
                    : std::string("--:--");
  const float curW = text_width(vg, L.timeFont, cur.c_str());
  const float leftW = text_width(vg, L.timeFont, left.c_str());

  // 左侧：播放 → 上一集 → 下一集（中心按 btnPitch 等距推进）
  float x = L.botBarPadX + L.btnHit * 0.5f;
  g.btnPlayX = x;
  x += L.btnPitch;
  g.btnPrevX = x;
  x += L.btnPitch;
  g.btnNextX = x;
  x += L.btnHit * 0.5f + L.botBarGap;
  g.timeCurX = x;
  x += curW + L.botBarGap;
  g.trackX = x;

  // 右侧：全屏 → 画中画 → 音量条 → 音量键（自右向左）
  x = w - L.botBarPadX;
  g.fsX = x - L.btnHit * 0.5f;
  x -= L.btnHit + L.botBarGap;
  g.pipX = x - L.btnHit * 0.5f;
  x -= L.btnHit + L.botBarGap;
  g.volW = L.volW;
  g.volX = x - g.volW;         // 条左端
  x -= g.volW + L.botBarGap;
  g.volIconX = x - L.btnHit * 0.5f;
  x -= L.btnHit + L.botBarGap;
  g.timeLeftX = x - leftW;
  g.trackW = std::max(40.f, g.timeLeftX - L.botBarGap - g.trackX);
  return g;
}

// —— 缩略图浮层布局（d59 自 chrome.cpp 绘制段原样提升，公式不得改动）——
ThumbLayout thumb_layout_of(NVGcontext* vg, const Theme& t, float w, float h, const Geom& g,
                            const PlaybackSnapshot& snap, float p, bool kb_mode) {
  const auto& L = t.layout;
  ThumbLayout tl;
  tl.w = L.thumbW;
  tl.h = L.thumbH;
  const float anchor_px = kb_mode ? (w * p) : (g.trackX + g.trackW * p);
  const float anchor_cy = kb_mode ? (h - L.miniLineH) : g.cy;
  tl.anchor_px = anchor_px;
  const float tx = anchor_px - L.thumbW * 0.5f;
  tl.tx = std::max(0.f, std::min(w - L.thumbW, tx));  // 越界才夹停（0 余量 = 可贴屏幕边）
  tl.ty = anchor_cy - L.thumbGapAbove - L.thumbH;     // 浮层底 = 锚线上方 gapAbove
  const std::string label = format_hms_seconds(p * snap.duration);
  tl.bubble_w = text_width(vg, L.timeFont, label.c_str()) + 10;
  tl.bubble_x = std::max(4.f, std::min(w - tl.bubble_w - 4.f, anchor_px - tl.bubble_w * 0.5f));
  return tl;
}

DmgRect thumb_band_of(const Theme& t, float w, float ty) {
  const auto& L = t.layout;
  const int top = (int)ty;  // 可为负（窗口过小浮层越出顶边，截到 0 并缩高度）
  const int bottom = (int)(ty + L.thumbH + 19.f);
  return DmgRect{0, top < 0 ? 0 : top, dm(w), bottom - (top < 0 ? 0 : top)};
}

// —— d74 剪贴板链接提示条布局（唯一真值：绘制/命中/归因/悬停同源）——
PromptLayout prompt_layout_of(NVGcontext* vg, const Theme& t, float w, float h,
                              bool fullscreen, double now, double at, const char* url,
                              bool enabled) {
  const auto& L = t.layout;
  PromptLayout pl;
  // 生命周期：[at, at+hold+fade)。clip_at=0 且 url 空 = 无提示（防御 URL 空串）。
  // enabled = main 侧 clip_visible（点播放/忽略后 main 置 false → 本帧即消失，
  // 归因走 prompt_visible 翻转差集；不吃它横幅会赖到 hold 结束，d74 首版踩坑）。
  if (!enabled) return pl;
  if (!url || !url[0]) return pl;
  const double age = now - at;
  if (age < 0 || age >= L.promptHoldSec + L.promptFadeSec) return pl;
  const float url_w = text_width(vg, L.promptFont, url);
  const float btns_w = L.promptBtnW * 2 + L.promptGap;
  float bar_w = L.promptPad * 2 + url_w + L.promptGap + btns_w;
  bar_w = std::min(bar_w, L.promptMaxW);
  bar_w = std::max(bar_w, L.promptPad * 2 + btns_w + 60.f);  // 极窄窗口下保按钮可用
  pl.w = bar_w;
  pl.h = L.promptBarH;
  pl.x = (w - bar_w) * 0.5f;
  pl.y = fullscreen ? 12.f : L.topBarH + 10.f;
  pl.btn_w = L.promptBtnW;
  pl.btn_h = L.promptBtnH;
  pl.btn_y = pl.y + (pl.h - pl.btn_h) * 0.5f;
  pl.btn_ignore_x = pl.x + pl.w - L.promptPad - L.promptBtnW;
  pl.btn_play_x = pl.btn_ignore_x - L.promptGap - L.promptBtnW;
  pl.url_x = pl.x + L.promptPad;
  pl.url_max_w = std::max(0.f, pl.btn_play_x - L.promptGap - pl.url_x);
  pl.visible = true;
  return pl;
}

UiRegions compute_regions(NVGcontext* vg, const Theme& t, float w, float h,
                          const PlaybackSnapshot& snap, const ViewInput& in,
                          bool media, bool pip, float fade, bool thumb_enabled) {
  const auto& L = t.layout;
  UiRegions R;
  // has_picture 与 render_loop 同式（picture_ready && 有 uri 的派生值）
  R.has_picture = snap.picture_ready && !snap.uri.empty();
  R.media = media;
  R.fullscreen = in.fullscreen;
  R.pip = pip;

  R.topbar = DmgRect{0, 0, dm(w), dm(L.topBarH)};
  R.bottom_bar = DmgRect{0, dm(h - L.botBarH), dm(w), dm(L.botBarH)};
  R.mini_line = DmgRect{0, dm(h - L.miniLineH), dm(w), dm(L.miniLineH)};

  // 中央大播放键：圆外接方（中心 (w/2, h/2)，与 center_play_hit 同一几何）
  const float cr = center_play_radius(t) * 0.5f;
  R.center_play = DmgRect{dm(w * 0.5f - cr), dm(h * 0.5f - cr), dm(cr * 2), dm(cr * 2)};
  // 可见性：只在暂停态绘制（player_view），画中画无 chrome（d57 同口径）
  R.center_visible = snap.state == TransportState::PausedPlayback && !pip;

  // 顶栏三键
  for (int i = 0; i < 3; i++) {
    const float bx = w - (float)(3 - i) * L.winBtnW;
    R.win_btn[i] = DmgRect{dm(bx), 0, dm(L.winBtnW), dm(L.topBarH)};
  }

  // 底栏控件级热区（y 范围与 bar_hit 同式：栏身 ∩ 按钮带）
  const Geom g = geom_of(vg, t, w, h, snap, media);
  const float hs = L.btnHit * 0.5f;
  const float y0 = std::max(g.cy - hs, g.botY);
  const float y1 = std::min(g.cy + hs, h);
  const float bh = y1 - y0;
  const float timeW = text_width(vg, L.timeFont, "--:--");
  R.bar_btn[kBtnPlay] = DmgRect{dm(g.btnPlayX - hs), dm(y0), dm(L.btnHit), dm(bh)};
  R.bar_btn[kBtnPrev] = DmgRect{dm(g.btnPrevX - hs), dm(y0), dm(L.btnHit), dm(bh)};
  R.bar_btn[kBtnNext] = DmgRect{dm(g.btnNextX - hs), dm(y0), dm(L.btnHit), dm(bh)};
  R.bar_btn[kBtnMute] = DmgRect{dm(g.volIconX - hs), dm(y0), dm(L.btnHit), dm(bh)};
  R.bar_btn[kBtnPip] = DmgRect{dm(g.pipX - hs), dm(y0), dm(L.btnHit), dm(bh)};
  R.bar_btn[kBtnFullscreen] = DmgRect{dm(g.fsX - hs), dm(y0), dm(L.btnHit), dm(bh)};
  R.bar_btn[kBtnWinMin] = R.win_btn[0];
  R.bar_btn[kBtnWinMax] = R.win_btn[1];
  R.bar_btn[kBtnWinClose] = R.win_btn[2];
  R.bar_btn[kBtnCenterPlay] = R.center_play;
  R.track = DmgRect{dm(g.trackX), dm(g.cy - 8), dm(g.trackW), 16};
  R.time_cur = DmgRect{dm(g.timeCurX - 2), dm(y0), dm(timeW + 6), dm(bh)};
  R.time_left = DmgRect{dm(g.timeLeftX - 4), dm(y0), dm(timeW + 6), dm(bh)};
  // 音量条 + 滑块包络（热区与 bar_hit.vol 同式：volHitPad 外扩 ±9px 纵向）
  R.volume = DmgRect{dm(g.volX - L.volHitPad), dm(g.cy - 9), dm(g.volW + L.volHitPad * 2), 18};

  // —— OSD 包络带（与 draw_osd 的 dmg_hit 包络同式：横全宽，纵向取两类面板最大高）——
  // 面板宽度自适应（含文本测量）对归因无意义：绘制早退也用这条保守带。
  R.osd_visible = !pip && in.osd_kind != 0 && in.osd_at > 0;
  if (R.osd_visible)
    R.osd = DmgRect{0, dm(L.osdTopGap), dm(w),
                    dm(std::max(L.osdVolH, L.osdSeekH) + L.osdTopGap)};

  // —— 缩略图浮层（可见性判定与 chrome.cpp 绘制段同口径）——
  // visible = 预览源就绪 && 有画面 && (条上悬停/拖拽/键盘预览) && alpha 不为 0
  const bool kb_active = in.kb_progress || in.now < in.kb_preview_until;
  const float kb_alpha = kb_active ? 1.f : 0.f;
  if (!pip && thumb_enabled && R.has_picture && snap.duration > 0 &&
      std::max(fade, kb_alpha) > 0.001f) {
    const BarHit hb = bar_hit(vg, t, w, h, snap, in, media);
    if (hb.track || in.drag_progress || in.kb_progress || in.now < in.kb_preview_until) {
      R.thumb_visible = true;
      const float p = in.drag_progress ? in.drag_value
                      : kb_active      ? in.kb_progress_value
                                       : progress_percent_at(in.mx, g.trackX, g.trackW);
      const bool kb_mode = kb_active && fade < 0.5f;  // 面板隐藏的键盘预览 = 迷你线几何
      const ThumbLayout tl = thumb_layout_of(vg, t, w, h, g, snap, p, kb_mode);
      R.thumb_card = thumb_band_of(t, w, tl.ty);
    }
  }

  // —— d74 剪贴板链接提示条（画中画小窗不画：无 chrome 语义，同 d57 口径）——
  {
    const PromptLayout pl =
        prompt_layout_of(vg, t, w, h, in.fullscreen, in.now, in.clip_at, in.clip_url,
                         in.clip_visible);
    R.prompt_visible = pl.visible && !pip;
    if (R.prompt_visible)
      R.prompt_bar = DmgRect{dm(pl.x), dm(pl.y), dm(pl.w), dm(pl.h)};
  }

  // ctx_menu / badges：render_loop 出帧路径补填（ctx_lay / 徽章文本在它手里）
  return R;
}

}  // namespace fr
