#include "ui/views/chrome.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <nanovg.h>

#include "app/event_bus.h"
#include "app/str_util.h"
#include "ui/regions.h"  // Geom/geom_of（v0.4.0 d56 提升：命中/绘制/归因同源）
#include "ui/theme.h"
#include "ui/views/player_view.h"  // ViewInput / ViewCallbacks
#include "ui/widgets.h"

namespace fr {

namespace {

bool in_rect(float px, float py, float x, float y, float w, float h) {
  return px >= x && px <= x + w && py >= y && py <= y + h;
}

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// 条端点与端点外扩：滑块中心正好落在两端时也不越出条身（translate(-50%,-50%) 铁律）
inline float vol_end_pad(const Theme& t) { return t.layout.volKnobHover * 0.5f; }

// d162：音量 ≤0 视同静音 —— 显示层唯一判定口径（静音按钮图标 / 音量条 / OSD 面板
// 全走这里）。协议上 Mute 是独立 A/V 状态变量，快照真值（muted / volume 两字段）
// 不动、SOAP 侧 GetMute 不受影响；本函数只服务视觉与交互观感。
inline bool eff_muted(const PlaybackSnapshot& s) { return s.muted || s.volume <= 0; }

}  // namespace

BarHit bar_hit(NVGcontext* vg, const Theme& t, float w, float h, const PlaybackSnapshot& snap,
               const ViewInput& in, bool media_enabled) {
  const auto& L = t.layout;
  const Geom g = geom_of(vg, t, w, h, snap, media_enabled);
  const float hs = L.btnHit * 0.5f;
  const float mx = in.mx, my = in.my;
  BarHit r;
  // 热区与栏身取交：光标移到栏上方一像素即视为离开（否则悬停底色会在栏外亮起）
  const float y0 = std::max(g.cy - hs, g.botY);
  const float y1 = std::min(g.cy + hs, h);
  const float bh = y1 - y0;

  // 时间文字区（与 --:-- 占位等宽，避免文字随内容伸缩时命中区抖动）
  const float timeW = text_width(vg, L.timeFont, "--:--");
  r.time = in_rect(mx, my, g.timeCurX - 2, y0, timeW + 6, bh) ||
           in_rect(mx, my, g.timeLeftX - 4, y0, timeW + 6, bh);

  r.play = in_rect(mx, my, g.btnPlayX - hs, y0, L.btnHit, bh);
  // d158：上/下一集 ↔ 停止键互斥（本应用无 playlist，"上一集"恒无数据源，
  // 两键是否可用整体取决于 next_uri）——无数据时两键不画、也不留隐形热区。
  const bool has_episode = !snap.next_uri.empty();
  r.prev = has_episode && in_rect(mx, my, g.btnPrevX - hs, y0, L.btnHit, bh);
  r.next = has_episode && in_rect(mx, my, g.btnNextX - hs, y0, L.btnHit, bh);
  r.stop = !has_episode && in_rect(mx, my, g.btnStopX - hs, y0, L.btnHit, bh);
  r.track = in_rect(mx, my, g.trackX, g.cy - 8, g.trackW, 16);
  r.track_x = g.trackX;
  r.track_w = g.trackW;
  r.speed = in_rect(mx, my, g.speedX - L.speedW * 0.5f, y0, L.speedW, bh);  // d169
  r.mute = in_rect(mx, my, g.volIconX - hs, y0, L.btnHit, bh);
  r.vol = in_rect(mx, my, g.volX - L.volHitPad, g.cy - 9, g.volW + L.volHitPad * 2, 18);
  r.pip = in_rect(mx, my, g.pipX - hs, y0, L.btnHit, bh);
  r.fs = in_rect(mx, my, g.fsX - hs, y0, L.btnHit, bh);

  // 媒体类控件在无会话时"视觉禁用"：不算被占用，点击由此落到"空白处"语义上。
  // 用局部副本判断，避免依赖下面 consumed 的推导顺序。
  if (!media_enabled) r.play = r.prev = r.next = r.stop = r.track = r.time = r.speed = false;

  // 点击消费：凡落在整条底栏上的按下都不该再触发"空白处 播放/暂停"
  r.consumed = r.any() || my >= g.botY;
  return r;
}

namespace {

// 条上位置 → 音量百分比。滑块中心锚在 [volX, volX+volW] 上，故分子要含端点内缩。
float vol_percent_at(float px, const Geom& g, const Theme& t) {
  const float pad = vol_end_pad(t);
  const float a = g.volX + pad, b = g.volX + g.volW - pad;
  return clamp01((px - a) / (b - a));
}

}  // namespace

float center_play_radius(const Theme& t) {
  return t.shape.playButtonSizeOverride > 0 ? t.shape.playButtonSizeOverride
                                            : t.layout.centerBtnSize;
}

float chrome_fade(NVGcontext* vg, const Theme& t, float w, float h, const PlaybackSnapshot& snap,
                  const ViewInput& in, bool media_enabled) {
  const auto& L = t.layout;
  (void)vg; (void)w; (void)snap; (void)media_enabled;
  const double idle_sec = in.now - in.last_input;
  // 拖拽进度/音量期间恒显（交互中不该淡出）
  if (in.drag_progress || in.drag_volume) return 1.f;
  // d203：鼠标停在操作栏区域 → 用更长的隐藏延迟（用户定 6s）；离开该区域恢复 1.5s。
  // 取代旧逻辑「悬停任一控件即永久显示」——那样永远不隐藏。
  const bool over_bar = in.my >= h - L.botBarH;
  const double hide_after = over_bar ? L.barHoverHideSec : L.idleHideSec;
  if (idle_sec < hide_after) return 1.f;
  if (idle_sec < hide_after + L.fadeSec)
    return 1.f - (float)((idle_sec - hide_after) / L.fadeSec);
  return 0.f;
}

void draw_win_buttons(NVGcontext* vg, const Theme& t, float w, ViewInput& in, ViewCallbacks& cb,
                      const DmgRect* clip) {
  const auto& L = t.layout;
  for (int i = 0; i < 3; i++) {
    const float bx = w - (3 - i) * L.winBtnW;
    const bool inside = in_rect(in.mx, in.my, bx, 0, L.winBtnW, L.topBarH);
    BtnState st = button_hit(in.btns, kBtnWinMin + i, inside, true, in.down, in.dt, t);
    const WinBtn kind = i == 0 ? WinBtn::Min : i == 1 ? WinBtn::Max : WinBtn::Close;
    // 只裁绘制，交互（button_hit/clicked）每帧全跑 —— 区域重绘的铁律
    if (dmg_hit(clip, bx, 0, L.winBtnW, L.topBarH)) {
      // 传 win_radius 与窗口宽：最外侧那角（关闭键的右上角）的悬停底色要跟着窗口圆角走，
      // 否则直角底色会盖住圆角、在角上露出一块方角。
      win_button(vg, t, bx, 0, L.winBtnW, L.topBarH, kind, st,
                 kind == WinBtn::Max && in.maximized, in.win_radius, w);
    }
    if (!st.clicked) continue;
    if (kind == WinBtn::Close && cb.on_close) cb.on_close();
    if (kind == WinBtn::Min && cb.on_minimize) cb.on_minimize();
    if (kind == WinBtn::Max && cb.on_maximize) cb.on_maximize();
  }
}

// —— d182：顶栏「皮肤按钮」几何（三键组左侧 skinBtnGap 处；绘制/命中同源）——
SkinBtnGeom skin_button_geom(const Theme& t, float win_w) {
  const auto& L = t.layout;
  SkinBtnGeom g;
  g.w = L.skinBtnSize;
  g.h = L.topBarH;
  g.y = 0;
  g.x = win_w - 3.f * L.winBtnW - L.skinBtnGap - g.w;
  return g;
}

void draw_skin_button(NVGcontext* vg, const Theme& t, float w, ViewInput& in, ViewCallbacks& cb,
                      const DmgRect* clip) {
  const auto& L = t.layout;
  const SkinBtnGeom g = skin_button_geom(t, w);
  const float cx = g.x + g.w * 0.5f;
  const float cy = L.topBarH * 0.5f;
  const bool inside = in.mx >= g.x && in.mx < g.x + g.w && in.my >= 0 && in.my < g.h;
  BtnState st = button_hit(in.btns, kBtnSkin, inside, true, in.down, in.dt, t);
  if (dmg_hit(clip, g.x, 0, g.w, g.h)) {
    btn_bg(vg, t, cx, cy, L.skinBtnSize, L.skinBtnSize, st);
    icon_skin(vg, cx, cy, L.skinBtnIcon * (1.f - (1.f - L.btnPressScale) * st.press),
              btn_icon_color(t, st));
  }
  if (st.clicked && cb.on_skin) cb.on_skin();
}

SkinMenuLayout skin_menu_layout(NVGcontext* vg, const Theme& t, float win_w, float win_h,
                                float anchor_x, float anchor_y,
                                const std::vector<std::string>& names) {
  const auto& L = t.layout;
  SkinMenuLayout lay;
  lay.count = (int)names.size();
  lay.item_h = L.ctxItemH;
  float maxw = 0.f;
  for (const auto& n : names) maxw = std::max(maxw, text_width(vg, L.titleFont, n.c_str()));
  lay.w = L.ctxPadX + L.ctxCheckW + maxw + L.ctxRightPad;
  lay.h = lay.count * lay.item_h + L.ctxPadY * 2;
  lay.x = std::min(std::max(2.f, anchor_x), std::max(2.f, win_w - lay.w - 2.f));
  lay.y = std::min(std::max(2.f, anchor_y + L.ctxAnchorOffset),
                   std::max(2.f, win_h - lay.h - 2.f));
  lay.ys.assign(lay.count, 0.f);
  for (int i = 0; i < lay.count; i++) lay.ys[i] = L.ctxPadY + i * lay.item_h;
  return lay;
}

int skin_menu_item_at(const SkinMenuLayout& lay, float px, float py) {
  if (px < lay.x || px > lay.x + lay.w || py < lay.y || py > lay.y + lay.h) return -1;
  for (int i = 0; i < lay.count; i++)
    if (py >= lay.y + lay.ys[i] && py <= lay.y + lay.ys[i] + lay.item_h) return i;
  return -1;
}

void draw_skin_menu(NVGcontext* vg, const Theme& t, const SkinMenuLayout& lay,
                    const std::vector<std::string>& names, int cur, int hover,
                    const DmgRect* clip) {
  const auto& L = t.layout;
  if (!dmg_hit(clip, lay.x, lay.y, lay.w, lay.h)) return;
  rounded_rect(vg, lay.x, lay.y, lay.w, lay.h, L.ctxRadius, t.barBg);
  for (int i = 0; i < lay.count; i++) {
    const float iy = lay.y + lay.ys[i];
    const bool hov = i == hover;
    if (hov)
      rounded_rect(vg, lay.x + 4.f, iy, lay.w - 8.f, lay.item_h, L.badgeRadius, t.btnHoverBg);
    const float cy = iy + lay.item_h * 0.5f;
    if (i == cur) {
      const float x0 = lay.x + L.ctxPadX + 4.f;
      nvgBeginPath(vg);
      nvgMoveTo(vg, x0, cy + 1.f);
      nvgLineTo(vg, x0 + 4.f, cy + 5.f);
      nvgLineTo(vg, x0 + 11.f, cy - 4.f);
      nvgStrokeColor(vg, t.played);
      nvgStrokeWidth(vg, 2.f);
      nvgLineCap(vg, NVG_ROUND);
      nvgStroke(vg);
    }
    const NVGcolor col = hov ? t.icon : t.subText;
    text(vg, t, lay.x + L.ctxPadX + L.ctxCheckW, cy, L.titleFont, col, names[i].c_str());
  }
}

void draw_bottom_bar(NVGcontext* vg, const Theme& t, float w, float h,
                     const PlaybackSnapshot& snap, ViewInput& in, ViewCallbacks& cb,
                     bool media_enabled, float fade, bool show_thumb_preview, float rad,
                     int thumb_img, const DmgRect* clip) {
  const auto& L = t.layout;
  const Geom g = geom_of(vg, t, w, h, snap, media_enabled);
  const BarHit hit = bar_hit(vg, t, w, h, snap, in, media_enabled);
  const float cy = g.cy;
  const bool paused = snap.state == TransportState::PausedPlayback;

  // fade == 0 时仍推进按钮动画（不画、也不可能有交互：光标在热区里必然使 fade = 1）
  nvgGlobalAlpha(vg, fade);
  bar(vg, t, 0, g.botY, w, L.botBarH, false, rad);

  // 任何控件被悬停/按下即视为"点击已被 chrome 消费"：空白处单击/双击判定据此排除
  if (hit.consumed) in.click_consumed = true;

  // 不在此处做窗口裁剪：底栏横跨整宽，唯一可能溢出的是窗口下方两个圆角，
  // 而那里本就不绘制任何子元素（按钮中心在栏垂直中线上，离底边有余量）。
  // 加裁剪反而引入"裁剪区算错 → 整条底栏连按钮一起消失"的风险（d17 踩过）。

  // —— 播放 / 暂停（媒体类：无会话禁用）——
  {
    BtnState st = button_hit(in.btns, kBtnPlay, hit.play, media_enabled, in.down, in.dt, t);
    const float k = round_btn_bg(vg, t, g.btnPlayX, cy, L.btnHit, st);
    const NVGcolor ic = btn_icon_color(t, st);
    if (paused)
      icon_play(vg, g.btnPlayX, cy, 12 * k, ic);
    else
      icon_pause(vg, g.btnPlayX, cy, 12 * k, ic);
    if (st.clicked && cb.on_play_pause) cb.on_play_pause();
  }

  // —— 上一集 / 下一集 ↔ 停止（媒体类；d158：无上/下一集数据时停止键替换两键）——
  if (!snap.next_uri.empty()) {
    {
      BtnState st = button_hit(in.btns, kBtnPrev, hit.prev, media_enabled, in.down, in.dt, t);
      const float k = round_btn_bg(vg, t, g.btnPrevX, cy, L.btnHit, st);
      icon_prev(vg, g.btnPrevX, cy, 12 * k, btn_icon_color(t, st));
      if (st.clicked && cb.on_prev) cb.on_prev();
    }
    {
      BtnState st = button_hit(in.btns, kBtnNext, hit.next, media_enabled, in.down, in.dt, t);
      const float k = round_btn_bg(vg, t, g.btnNextX, cy, L.btnHit, st);
      icon_next(vg, g.btnNextX, cy, 12 * k, btn_icon_color(t, st));
      if (st.clicked && cb.on_next) cb.on_next();
    }
  } else {
    BtnState st = button_hit(in.btns, kBtnStop, hit.stop, media_enabled, in.down, in.dt, t);
    const float k = round_btn_bg(vg, t, g.btnStopX, cy, L.btnHit, st);
    icon_stop(vg, g.btnStopX, cy, 12 * k, btn_icon_color(t, st));
    if (st.clicked && cb.on_stop) cb.on_stop();
  }

  // —— 时间（无媒体显示占位；禁用时降一档亮度）——
  // d37：键盘拖动预览改由常驻迷你线承担（贴底全宽细线），底栏时间/进度条
  // 不再为键盘点亮 —— 键盘操作在面板隐藏时只动迷你线 + 预览卡。
  {
    nvgGlobalAlpha(vg, fade);
    const NVGcolor tc = media_enabled ? t.subText : t.mutedText;
    const std::string cur =
        media_enabled ? format_hms_seconds(snap.position) : std::string("--:--");
    const std::string left =
        media_enabled ? "-" + format_hms_seconds(std::max(0.0, snap.duration - snap.position))
                      : std::string("--:--");
    text(vg, t, g.timeCurX, cy, L.timeFont, tc, cur.c_str());
    text(vg, t, g.timeLeftX, cy, L.timeFont, tc, left.c_str());
  }

  // —— 进度条（媒体类：无会话不可拖）——
  {
    const bool kb_active = in.kb_progress || in.now < in.kb_preview_until;
    float percent = 0.f;
    if (in.drag_progress)
      percent = in.drag_value;
    else if (kb_active)
      percent = in.kb_progress_value;  // 键盘拖动：条动但**不** seek（抬起才提交）
    else if (snap.duration > 0)
      percent = clamp01((float)(snap.position / snap.duration));
    // 缓冲层 mpv 侧未接（cache-buffered），M1 不画
    progress_track(vg, t, g.trackX, cy, g.trackW, percent, -1.f,
                   media_enabled && (hit.track || in.drag_progress || in.kb_progress));

    if (in.drag_progress) {
      in.drag_value = progress_percent_at(in.mx, g.trackX, g.trackW);  // 拖动中实时跟随
      if (!in.down) {
        in.drag_progress = false;
        if (cb.on_seek && media_enabled && snap.duration > 0)
          cb.on_seek(in.drag_value * snap.duration);
      }
    } else if (media_enabled && hit.track && in.down && !in.drag_volume) {
      in.drag_progress = true;
      in.drag_value = progress_percent_at(in.mx, g.trackX, g.trackW);
    }
  }

  // —— 倍速（d169，媒体类：无会话禁用；点击弹出档位列表，列表绘制在 render_loop）——
  {
    BtnState st = button_hit(in.btns, kBtnSpeed, hit.speed, media_enabled, in.down, in.dt, t);
    const float k = pill_btn_bg(vg, t, g.speedX, cy, L.speedW, L.btnBoxW, st);
    const NVGcolor col = media_enabled ? btn_icon_color(t, st) : t.iconDisabled;
    text(vg, t, g.speedX, cy, L.speedFont * k, col, format_speed(snap.speed).c_str(),
         NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    if (st.clicked && cb.on_speed) cb.on_speed();
  }

  // —— 音量图标 = 静音键（窗口类：任何状态可用；中键与左键同义，见 main）——
  {
    BtnState st = button_hit(in.btns, kBtnMute, hit.mute, true, in.down, in.dt, t);
    const float k = round_btn_bg(vg, t, g.volIconX, cy, L.btnHit, st);
    const bool shown_muted = eff_muted(snap);  // d162：音量 ≤0 视同静音显示
    const NVGcolor ic = shown_muted ? t.muteIcon : btn_icon_color(t, st);
    if (shown_muted)
      icon_volume_muted(vg, g.volIconX, cy, L.volIcon * k, ic, ic);
    else
      icon_volume(vg, g.volIconX, cy, L.volIcon * k, ic);
    if (st.clicked && cb.on_mute) cb.on_mute(!snap.muted);
  }

  // —— 音量条（窗口类：任何状态可用；带滑块，拖动自动解除静音）——
  {
    const bool held = hit.vol && in.down;
    const float knob_r = (held || (hit.vol && in.drag_volume)) ? L.volKnobHover * 0.5f
                                                              : L.volKnob * 0.5f;
    const float vp = in.drag_volume ? in.drag_value : clamp01((float)snap.volume / 100.f);
    volume_bar(vg, t, g.volX + g.volW * 0.5f, cy, g.volW, vp, knob_r, eff_muted(snap));
    if (in.drag_volume) {
      in.drag_value = vol_percent_at(in.mx, g, t);
      if (!in.down) {
        in.drag_volume = false;
        // 拖动即表态要出声：静音中被拖动 → 先解除静音再落音量（否则拖了没反应）；
        // d162：音量 0 的"视同静音"同样适用 —— 拖上去即恢复有声观感。
        if (eff_muted(snap) && cb.on_mute) cb.on_mute(false);
        if (cb.on_volume) cb.on_volume(in.drag_value);
      }
    } else if (hit.vol && in.down && !in.drag_progress && in.btns.press_id < 0) {
      in.drag_volume = true;
      in.drag_value = vol_percent_at(in.mx, g, t);
    }
  }

  // —— 画中画 / 全屏（窗口类：任何状态可用）——
  {
    BtnState st = button_hit(in.btns, kBtnPip, hit.pip, true, in.down, in.dt, t);
    const float k = round_btn_bg(vg, t, g.pipX, cy, L.btnHit, st);
    icon_pip(vg, g.pipX, cy, 12 * k, btn_icon_color(t, st));
    if (st.clicked && cb.on_pip) cb.on_pip();
  }
  {
    BtnState st = button_hit(in.btns, kBtnFullscreen, hit.fs, true, in.down, in.dt, t);
    const float k = round_btn_bg(vg, t, g.fsX, cy, L.btnHit, st);
    icon_fullscreen(vg, g.fsX, cy, 12 * k, btn_icon_color(t, st));
    if (st.clicked && cb.on_fullscreen) cb.on_fullscreen();
  }

  // —— 缩略图浮层（悬停/拖拽/键盘拖动进度时；进度点锚 + translateX(-50%)）——
  // d37 用户定值：键盘预览卡（面板隐藏）锚**常驻迷你线** —— 全宽几何 px = w*p，
  // 与迷你线进度点对齐，卡能一路跟到屏幕最右；只有卡右缘将超出屏幕时才夹停，
  // 否则始终跟随。悬停/鼠标拖拽仍锚面板进度条（面板亮）。时间气泡屏内夹取不变。
  const float kb_alpha = (in.kb_progress || in.now < in.kb_preview_until) ? 1.f : 0.f;
  if (show_thumb_preview && std::max(fade, kb_alpha) > 0.001f &&
      (hit.track || in.drag_progress || in.kb_progress || in.now < in.kb_preview_until)) {
    const bool kb_active = in.kb_progress || in.now < in.kb_preview_until;
    nvgGlobalAlpha(vg, std::max(fade, kb_alpha));
    const bool kb_mode = kb_active && fade < 0.5f;  // 面板隐藏的键盘预览 = 迷你线几何
    const float p = in.drag_progress ? in.drag_value
                    : kb_active      ? in.kb_progress_value
                                     : progress_percent_at(in.mx, g.trackX, g.trackW);
    // d59 几何单点化：布局公式收进 regions.cpp thumb_layout_of（绘制/归因同源）
    const ThumbLayout tl = thumb_layout_of(vg, t, w, h, g, snap, p, kb_mode);
    const DmgRect band = thumb_band_of(t, w, tl.ty);
    // 浮层卡 + 下方时间气泡的包络（横向取全宽：气泡 x 随锚点移动）
    if (dmg_hit(clip, (float)band.x, (float)band.y, (float)band.w, (float)band.h)) {
    rounded_rect(vg, tl.tx, tl.ty, tl.w, tl.h, L.thumbRadius, t.previewBg);
    // 悬停点画面（thumb worker 已出图时贴真画面；圆角 path 填充天然裁出圆角）。
    // jpg 是标准自上而下编码，nanovg 内存加载方向一致，**不翻**（与主画面 FBO 不可比）。
    if (thumb_img > 0) {
      nvgBeginPath(vg);
      nvgRoundedRect(vg, tl.tx, tl.ty, tl.w, tl.h, L.thumbRadius);
      nvgFillPaint(vg, nvgImagePattern(vg, tl.tx, tl.ty, tl.w, tl.h, 0, thumb_img, 1.0f));
      nvgFill(vg);
    }
    // d215：缩略图外描边线已去掉（一律不留描边）
    rounded_rect(vg, tl.bubble_x, tl.ty + L.thumbH + 3, tl.bubble_w, 16, 3, t.previewTimeBg);
    text(vg, t, tl.anchor_px, tl.ty + L.thumbH + 11, L.timeFont, t.previewTimeText,
         format_hms_seconds(p * snap.duration).c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    }  // dmg_hit(缩略图浮层包络)
  }

  nvgGlobalAlpha(vg, 1.f);
}

// —— 常驻迷你进度线（d37 用户定值）：贴底、全宽（以窗口宽度为进度参考）、
// 收窄 2px、面板隐去后**不消失** —— 无面板时用户仍可感知播放进度。
// 低调：alpha = (1 - fade) × miniLineMaxAlpha，面板出现时让位给完整进度条。
// 键盘拖动（kb）时由 kb_progress_value 驱动（预览跟随），释放 0.6s 后回真实位置。
void draw_mini_progress(NVGcontext* vg, const Theme& t, float w, float h,
                        const PlaybackSnapshot& snap, const ViewInput& in, float fade,
                        const DmgRect* clip) {
  if (snap.duration <= 0) return;
  const auto& L = t.layout;
  // 纯绘制段：整带早退安全（无交互语义）
  if (!dmg_hit(clip, 0.f, h - L.miniLineH, w, L.miniLineH)) return;
  const float p = (in.kb_progress || in.now < in.kb_preview_until)
                      ? clamp01(in.kb_progress_value)
                      : clamp01((float)(snap.position / snap.duration));
  const float alpha = (1.f - fade) * L.miniLineMaxAlpha;
  if (alpha < 0.02f) return;
  nvgGlobalAlpha(vg, alpha);
  const float y = h - L.miniLineH;
  nvgBeginPath(vg);
  nvgRect(vg, 0, y, w, L.miniLineH);
  nvgFillColor(vg, t.track);
  nvgFill(vg);
  if (p > 0.001f) {
    nvgBeginPath(vg);
    nvgRect(vg, 0, y, w * p, L.miniLineH);
    nvgFillColor(vg, t.played);
    nvgFill(vg);
  }
  nvgGlobalAlpha(vg, 1.f);
}

// —— OSD 浮层（d28；d29 精修）：出现 0.15s → 保持 1.2s → 淡出 0.3s；纯时间推算 ——
// 触发源：方向键/滚轮音量、音量条拖拽释放（Volume intent）、seek（进度条拖拽释放、
// 左右方向键 ±5s）。位置 = 左上角四分之一处（用户定值，theme.osdAnchor*）。
// 音量 = 竖向面板（喇叭在上、条居中、百分比在下）；进度 = 横向面板（方向图标 + 目标时间）。
void draw_osd(NVGcontext* vg, const Theme& t, float w, float h, const PlaybackSnapshot& snap,
              const ViewInput& in, const DmgRect* clip) {
  const auto& L = t.layout;
  if (in.osd_kind == 0 || in.osd_at <= 0) return;
  // 纯绘制段：OSD 面板固定贴左上（osdEdgeGap/osdTopGap），保守包络 = 左上角带
  //（纵向 max(音量面板,横向面板) 高度，横向全宽 —— kind2/3 面板宽随文字自适应）
  if (!dmg_hit(clip, 0.f, L.osdTopGap, w, std::max(L.osdVolH, L.osdSeekH))) return;
  const double t0 = in.now - in.osd_at;
  const double total = L.osdFadeIn + L.osdHold + L.osdFadeOut;
  if (t0 < 0 || t0 >= total) return;
  float alpha = 1.f;
  if (t0 < L.osdFadeIn)
    alpha = (float)(t0 / L.osdFadeIn);
  else if (t0 > L.osdFadeIn + L.osdHold)
    alpha = 1.f - (float)((t0 - L.osdFadeIn - L.osdHold) / L.osdFadeOut);
  nvgGlobalAlpha(vg, clamp01(alpha));

  if (in.osd_kind == 1) {
    // —— 音量（竖向）：喇叭 / 竖条（自下而上填充）/ 百分比 ——
    const float px = L.osdEdgeGap;                                  // 贴左缘（用户定值）
    const float py = L.osdTopGap;                                   // 固定顶距（d35）
    const float cx = px + L.osdVolW * 0.5f;
    rounded_rect(vg, px, py, L.osdVolW, L.osdVolH, L.osdRadius, t.barBg);
    icon_volume(vg, cx, py + L.osdVolIconY, L.osdVolIconR,
                eff_muted(snap) ? t.muteIcon : t.icon);
    // 竖条：轨道圆角矩形，填充自底向上（进度语义：底=0 顶=100）
    // 布局（d31 用户反馈：文字与条重叠/贴边不居中）：图标带 py+15..29，条 py+40..110，
    // 文字中线 py+132（上距条底 15px、下距面板底 11px、两侧到图标带对称）
    const float bx = cx - L.osdBarThick * 0.5f;
    const float by = py + L.osdBarTopY;
    rounded_rect(vg, bx, by, L.osdBarThick, L.osdBarLen, L.osdBarThick * 0.5f,
                 eff_muted(snap) ? t.muteTrack : t.track);
    const float fill = L.osdBarLen * clamp01((float)(in.osd_value / 100.0));
    if (fill > 0.5f)
      rounded_rect(vg, bx, by + L.osdBarLen - fill, L.osdBarThick, fill, L.osdBarThick * 0.5f,
                   eff_muted(snap) ? t.muteKnob : t.played);
    const std::string pct = std::to_string((int)(in.osd_value + 0.5)) + "%";
    text(vg, t, cx, py + L.osdVolH - L.osdPctBottom, L.osdFont, t.subText, pct.c_str(),
         NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
  } else if (in.osd_kind == 2 || in.osd_kind == 3) {
    // —— 横向面板（d32 重写，kind2 进度 / kind3 动作反馈共用骨架）——
    // 布局：图标中心 px+30，右缘留白 24；面板宽 = 30+9+18+文本宽+24（自适应，
    // 取代固定 osdSeekW，用户反馈"间距不对需要打磨"）。
    // kind2 方向图标（d32 修复）：绘制时只认触发时定格的 in.osd_dir（1 前进/0 后退），
    // 不再比对实时 position —— 旧逻辑 seek 完成后 position==target 恒判后退，
    // 右方向键也显示左箭头。
    // kind3 动作反馈（d32 新增）：osd_value 编码动作 id —— 1 播放 2 暂停
    // 3 进全屏 4 退全屏 5 进画中画 6 退画中画 7 停止（d158，停止键反馈）。
    const float px = L.osdEdgeGap;                                  // 贴左缘（用户定值）
    // 垂直锚点（d35）：与音量面板同顶边 = 固定顶距 osdTopGap，不随窗口高度缩放
    //（d32 曾按 h*0.25 中心/顶边换算，窗口大时面板位置漂移，用户定值废除）。
    const float py = L.osdTopGap;
    // d160：小窗口等比收缩 —— 面板高超过窗口高 × osdMaxHRatio 时整体缩（图标/字号/
    // 间距同比例），恒 ≤1：大窗口不放大，保持 d32/d35 定稿视觉。音量竖面板不参与。
    const float s = std::min(1.f, h * L.osdMaxHRatio / L.osdSeekH);
    const float ph = L.osdSeekH * s;
    const float cy = py + ph * 0.5f;
    const float icon_cx = px + L.osdIconPad * s;
    const float icon_half = L.osdIconHalf * s;
    const float gap = L.osdTextGap * s;
    const float right_pad = L.osdRightPad * s;

    const char* label = nullptr;
    std::string text_buf;
    if (in.osd_kind == 2) {
      text_buf = format_hms_seconds(std::max(0.0, in.osd_value));
      label = text_buf.c_str();
    } else {
      const int id = (int)(in.osd_value + 0.5);
      switch (id) {
        case 1: label = "播放"; break;
        case 2: label = "暂停"; break;
        case 3: label = "全屏"; break;
        case 4: label = "退出全屏"; break;
        case 5: label = "画中画"; break;
        case 6: label = "退出画中画"; break;
        case 7: label = "停止"; break;
        default: label = ""; break;
      }
    }

    const float tw = text_width(vg, L.osdFont * s, label);
    const float pw = icon_cx - px + icon_half + gap + tw + right_pad;
    rounded_rect(vg, px, py, pw, ph, L.osdRadius * s, t.barBg);

    if (in.osd_kind == 2) {
      if (in.osd_dir >= 1)
        icon_next(vg, icon_cx, cy, L.osdIconR * s, t.icon);
      else
        icon_prev(vg, icon_cx, cy, L.osdIconR * s, t.icon);
    } else {
      const int id = (int)(in.osd_value + 0.5);
      if (id == 1) icon_play(vg, icon_cx, cy, L.osdActIconR * s, t.icon);
      else if (id == 2) icon_pause(vg, icon_cx, cy, L.osdActIconR * s, t.icon);
      else if (id == 3 || id == 4) icon_fullscreen(vg, icon_cx, cy, L.osdActIconR * s, t.icon);
      else if (id == 5 || id == 6) icon_pip(vg, icon_cx, cy, L.osdActIconR * s, t.icon);
      else if (id == 7) icon_stop(vg, icon_cx, cy, L.osdIconR * s, t.icon);
    }

    text(vg, t, icon_cx + icon_half + gap, cy, L.osdFont * s, t.subText, label,
         NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
  } else if (in.osd_kind == 4) {
    // —— 倍速反馈（d169）：横向面板，居中显示「倍速 1.5x」；无图标 ——
    const float px = L.osdEdgeGap;
    const float py = L.osdTopGap;
    const float s = std::min(1.f, h * L.osdMaxHRatio / L.osdSeekH);
    const float ph = L.osdSeekH * s;
    const std::string label = "倍速 " + format_speed(in.osd_value);
    const float tw = text_width(vg, L.osdFont * s, label.c_str());
    const float pw = tw + L.osdSpeedPad * s;
    rounded_rect(vg, px, py, pw, ph, L.osdRadius * s, t.barBg);
    text(vg, t, px + pw * 0.5f, py + ph * 0.5f, L.osdFont * s, t.subText, label.c_str(),
         NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
  }
  nvgGlobalAlpha(vg, 1.f);
}

// —— 右键上下文菜单（d45）——
// 几何常量说明：菜单是临时浮层，尺寸常量取现有 token 就近组合（osdRadius 面板圆角 /
// badgeRadius 悬停条圆角 / titleFont 条目字号），不为它新增 layout token。
int CtxMenuLayout::item_at(float px, float py) const {
  if (px < x || px > x + w || py < y || py > y + h) return -1;
  for (int i = 0; i < count; i++) {
    if (sep[i]) continue;
    if (py >= y + ys[i] && py <= y + ys[i] + item_h) return i;
  }
  return -1;
}

CtxMenuLayout ctx_menu_layout(NVGcontext* vg, const Theme& t, float win_w, float win_h,
                              const CtxMenuItem* items, int n, float mx, float my) {
  const auto& L = t.layout;
  CtxMenuLayout lay;
  lay.count = n;
  lay.item_h = L.ctxItemH;
  lay.sep_h = L.ctxSepH;
  lay.pad_y = L.ctxPadY;
  float maxw = 0.f, total = lay.pad_y;
  for (int i = 0; i < n && i < 16; i++) {
    lay.sep[i] = items[i].label == nullptr;
    lay.ys[i] = total;
    total += lay.sep[i] ? lay.sep_h : lay.item_h;
    if (!lay.sep[i]) maxw = std::max(maxw, text_width(vg, L.titleFont, items[i].label));
  }
  lay.w = L.ctxPadX + L.ctxCheckW + maxw + L.ctxRightPad;
  lay.h = total + lay.pad_y;
  // 弹出点：锚点右下偏 ctxAnchorOffset；越界回夹进窗口（四边各留 2px）
  lay.x = std::min(std::max(2.f, mx + L.ctxAnchorOffset),
                   std::max(2.f, win_w - lay.w - 2.f));
  lay.y = std::min(std::max(2.f, my + L.ctxAnchorOffset),
                   std::max(2.f, win_h - lay.h - 2.f));
  return lay;
}

void draw_ctx_menu(NVGcontext* vg, const Theme& t, const CtxMenuLayout& lay,
                   const CtxMenuItem* items, int n, int hover, const DmgRect* clip) {
  const auto& L = t.layout;
  // 纯绘制段：面板矩形现成（lay），整面板早退安全
  if (!dmg_hit(clip, lay.x, lay.y, lay.w, lay.h)) return;
  rounded_rect(vg, lay.x, lay.y, lay.w, lay.h, L.ctxRadius, t.barBg);
  for (int i = 0; i < n && i < lay.count; i++) {
    const float iy = lay.y + lay.ys[i];
    if (lay.sep[i]) {
      const float sy = iy + lay.sep_h * 0.5f;
      nvgBeginPath(vg);
      nvgMoveTo(vg, lay.x + 10.f, sy);
      nvgLineTo(vg, lay.x + lay.w - 10.f, sy);
      nvgStrokeColor(vg, t.track);
      nvgStrokeWidth(vg, 1.f);
      nvgStroke(vg);
      continue;
    }
    const bool hov = i == hover && items[i].enabled;
    if (hov)
      rounded_rect(vg, lay.x + 4.f, iy, lay.w - 8.f, lay.item_h, L.badgeRadius, t.btnHoverBg);
    const float cy = iy + lay.item_h * 0.5f;
    if (items[i].checkable && items[i].checked) {
      // 手绘 ✓（两段线段；不依赖字体字形，禁用态也保持原勾选可见性）
      const float x0 = lay.x + L.ctxPadX + 4.f;
      nvgBeginPath(vg);
      nvgMoveTo(vg, x0, cy + 1.f);
      nvgLineTo(vg, x0 + 4.f, cy + 5.f);
      nvgLineTo(vg, x0 + 11.f, cy - 4.f);
      nvgStrokeColor(vg, t.played);
      nvgStrokeWidth(vg, 2.f);
      nvgLineCap(vg, NVG_ROUND);
      nvgStroke(vg);
    }
    // 禁用色用 iconDisabled（30% 白，与底栏图标禁用同一 token）：
    // mutedText(#9aa0aa) 与 subText(#babec6) 太接近，视觉上分不出禁用态（d45 用户实测）
    const NVGcolor col = items[i].enabled ? (hov ? t.icon : t.subText) : t.iconDisabled;
    text(vg, t, lay.x + L.ctxPadX + L.ctxCheckW, cy, L.titleFont, col, items[i].label);
  }
}

// —— d169 倍速档位（唯一真值）——
namespace {
const double kSpeedPresets[kSpeedPresetCount] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0};
}  // namespace

double speed_preset(int index) {
  if (index < 0 || index >= kSpeedPresetCount) return 1.0;
  return kSpeedPresets[index];
}

int speed_preset_nearest(double s) {
  int best = 0;
  double bd = 1e9;
  for (int i = 0; i < kSpeedPresetCount; i++) {
    const double d = std::fabs(kSpeedPresets[i] - s);
    if (d < bd) {
      bd = d;
      best = i;
    }
  }
  return best;
}

SpeedMenuLayout speed_menu_layout(NVGcontext* vg, const Theme& t, float win_w, float win_h,
                                  float anchor_cx, float bot_y) {
  const auto& L = t.layout;
  SpeedMenuLayout lay;
  lay.count = kSpeedPresetCount;
  lay.item_h = L.speedItemH;
  float maxw = 0.f;
  for (int i = 0; i < lay.count; i++)
    maxw = std::max(maxw, text_width(vg, L.speedFont, format_speed(speed_preset(i)).c_str()));
  lay.w = L.ctxPadX + L.ctxCheckW + maxw + L.ctxRightPad;
  lay.h = lay.count * lay.item_h + L.speedMenuPadY * 2;
  lay.x = std::min(std::max(2.f, anchor_cx - lay.w * 0.5f),
                   std::max(2.f, win_w - lay.w - 2.f));
  lay.y = std::max(2.f, bot_y - 6.f - lay.h);  // 贴底栏上方 6px
  for (int i = 0; i < lay.count; i++) lay.ys[i] = L.speedMenuPadY + i * lay.item_h;
  (void)win_h;
  return lay;
}

int speed_menu_item_at(const SpeedMenuLayout& lay, float px, float py) {
  if (px < lay.x || px > lay.x + lay.w || py < lay.y || py > lay.y + lay.h) return -1;
  for (int i = 0; i < lay.count; i++) {
    if (py >= lay.y + lay.ys[i] && py <= lay.y + lay.ys[i] + lay.item_h) return i;
  }
  return -1;
}

void draw_speed_menu(NVGcontext* vg, const Theme& t, const SpeedMenuLayout& lay,
                     double cur_speed, int hover, const DmgRect* clip) {
  const auto& L = t.layout;
  if (!dmg_hit(clip, lay.x, lay.y, lay.w, lay.h)) return;
  rounded_rect(vg, lay.x, lay.y, lay.w, lay.h, L.ctxRadius, t.barBg);
  const int cur = speed_preset_nearest(cur_speed);
  for (int i = 0; i < lay.count; i++) {
    const float iy = lay.y + lay.ys[i];
    const bool hov = i == hover;
    if (hov)
      rounded_rect(vg, lay.x + 4.f, iy, lay.w - 8.f, lay.item_h, L.badgeRadius, t.btnHoverBg);
    const float cy = iy + lay.item_h * 0.5f;
    if (i == cur) {  // 当前档位打勾（与右键菜单同一手绘勾选样式）
      const float x0 = lay.x + L.ctxPadX + 4.f;
      nvgBeginPath(vg);
      nvgMoveTo(vg, x0, cy + 1.f);
      nvgLineTo(vg, x0 + 4.f, cy + 5.f);
      nvgLineTo(vg, x0 + 11.f, cy - 4.f);
      nvgStrokeColor(vg, t.played);
      nvgStrokeWidth(vg, 2.f);
      nvgLineCap(vg, NVG_ROUND);
      nvgStroke(vg);
    }
    const NVGcolor col = hov ? t.icon : t.subText;
    text(vg, t, lay.x + L.ctxPadX + L.ctxCheckW, cy, L.speedFont, col,
         format_speed(speed_preset(i)).c_str());
  }
}

}  // namespace fr
