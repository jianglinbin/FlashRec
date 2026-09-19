#include "ui/views/player_view.h"

#include <algorithm>
#include <cstdlib>

#include <nanovg.h>

#include "app/event_bus.h"
#include "app/log.h"
#include "ui/theme.h"
#include "ui/views/chrome.h"
#include "ui/widgets.h"

namespace fr {

namespace {

bool in_rect(float px, float py, float x, float y, float w, float h) {
  return px >= x && px <= x + w && py >= y && py <= y + h;
}

}  // namespace

void draw_player_view(NVGcontext* vg, const Theme& t, float w, float h,
                      const PlaybackSnapshot& snap, ViewInput& in, ViewCallbacks& cb,
                      int thumb_img, const DmgRect* clip) {
  const auto& L = t.layout;
  if (snap.uri.empty()) return;  // 无媒体：待机画面由 empty_state 负责

  const bool has_media = snap.has_session;  // 有会话（含停止后的进度保持期）
  const bool paused = snap.state == TransportState::PausedPlayback;
  const float fade = chrome_fade(vg, t, w, h, snap, in, has_media);
  const float rad = in.win_radius;  // 全屏 / 最大化 = 0（直角）

  // —— 顶栏（窗口模式常显：不透明、不参与 idle 淡出；全屏时整个不渲染）——
  // 底色/文字/徽章是纯绘制段可裁；三键交互在 draw_win_buttons 内部每帧全跑
  if (!in.fullscreen) {
    if (dmg_hit(clip, 0.f, 0.f, w, L.topBarH)) {
      bar(vg, t, 0, 0, w, L.topBarH, true, rad, &t.topBarBg);
      icon_play(vg, L.topBarPadL + L.titleIconGap, L.topBarH * 0.5f, 9, t.icon);
      text_truncated(vg, t, L.topBarPadL + L.titleTextGap, L.topBarH * 0.5f, L.titleFont,
                     t.titleText, snap.title.c_str(), w * L.titleMaxRatio);
      // 徽章列（d124 起可含多枚）：来源徽章 + 软件渲染兜底提示。
      // 左起位置 = 标题截断后的实际宽度 + 间距；空间不足则**逐个省略**（不挤压按钮区）。
      float bx = L.topBarPadL + L.titleTextGap +
                 std::min(w * L.titleMaxRatio, text_width(vg, L.titleFont, snap.title.c_str())) +
                 L.titleIconGap + L.badgeMarginL;
      const float bx_limit = w - 3 * L.winBtnW - 6;
      auto badge = [&](const char* s) {
        const float bw = text_width(vg, L.badgeFont, s) + L.badgePadX * 2;
        if (bx + bw >= bx_limit) return;
        rounded_rect(vg, bx, L.topBarH * 0.5f - L.badgeFont * 0.5f - L.badgePadY, bw,
                     L.badgeFont + L.badgePadY * 2, L.badgeRadius, t.badgeBg);
        text(vg, t, bx + L.badgePadX, L.topBarH * 0.5f, L.badgeFont, t.badgeText, s);
        bx += bw + L.badgeMarginL;
      };
      // 来源徽章（M1 固定文案；来源 App 识别 = M2，从 User-Agent/DIDL 解析）
      if (!snap.metadata.empty()) badge("DLNA 投屏");
      // d124：软件渲染兜底提示 —— 让用户知道"画面是 CPU 出的、硬件加速没生效"，
      // 而不是看不出区别（老显卡驱动编译不了 mpv 的 GLSL 时会自动降级到这条路）
      if (in.sw_video) badge("软件渲染");
    }
    draw_win_buttons(vg, t, w, in, cb, clip);
    draw_skin_button(vg, t, w, in, cb, clip);  // d182：顶栏皮肤按钮
  }

  // —— 底栏 + 悬停缩略图浮层（与待机态共用一份实现；缩略图仅播放态有）——
  draw_bottom_bar(vg, t, w, h, snap, in, cb, has_media, fade, true, rad, thumb_img, clip);
  draw_mini_progress(vg, t, w, h, snap, in, fade, clip);  // d37：常驻迷你进度线（面板隐去后仍显示）

  // —— OSD 反馈浮层（键/滚轮/拖拽释放；不参与 chrome 淡出）——
  draw_osd(vg, t, w, h, snap, in, clip);

  // —— 中央大播放键（暂停时常显；容器仍为圆形，与底栏按钮的方形底色区分开）——
  // 交互（button_hit/inside/clicked）每帧全跑，只裁绘制
  if (paused) {
    const float base = center_play_radius(t);
    const float r = base * 0.5f;
    const bool inside =
        in_rect(in.mx, in.my, w * 0.5f - r, h * 0.5f - r, base, base);
    BtnState st = button_hit(in.btns, kBtnCenterPlay, inside, true, in.down, in.dt, t);
    if (inside) in.click_consumed = true;
    if (dmg_hit(clip, w * 0.5f - r, h * 0.5f - r, base, base)) {
      // 悬停微放大 / 按压缩小（不换色，避免与各皮肤中央键本色冲突）
      const float grow = 1.f + (1.f - L.btnPressScale) * 0.5f * st.hover;
      const float k = grow * (1.f - (1.f - L.btnPressScale) * st.press);
      circle_btn(vg, t, w * 0.5f, h * 0.5f, base * k, t.playButtonBg, t.playButtonBorder,
                 t.playButtonIcon);
      icon_play(vg, w * 0.5f, h * 0.5f, L.centerIconSize * k, t.playButtonIcon);
    }
    if (st.clicked && cb.on_play_pause) cb.on_play_pause();
  }
}

}  // namespace fr
